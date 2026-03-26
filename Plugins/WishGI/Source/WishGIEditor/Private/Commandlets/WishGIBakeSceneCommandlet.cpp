#include "Commandlets/WishGIBakeSceneCommandlet.h"

#include "AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

#include "WishGIMeshAssocAsset.h"
#include "WishGIProbeMapAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogWishGIBakeScene, Log, All);

namespace WishGIBakeScene
{
	// 中文说明：
	// 1) 本文件负责离线 Bake 阶段的数据求解与资产打包。
	// 2) 当前为 MVP 求解框架：由顶点-探针关联构建线性系统，再用 CG 求解 RGB 信号。
	// 3) 后续接入真实光照采样时，优先替换 BuildVertexTargets/BuildSystems 的目标项构造。
	struct FSettings
	{
		FString MapPath;
		FString AssocPath = TEXT("/Game/WishGI/MeshAssoc");
		FString OutPath = TEXT("/Game/WishGI/Bake");
		FString AssetName = TEXT("WishGI_ProbeMap");
		int32 SHOrder = 2;
		int32 Directions = 192;
		float Lambda = 0.1f;
		bool bOverwrite = false;
		bool bHelp = false;
	};

	struct FLinearSystemDense
	{
		int32 Size = 0;
		TArray<double> A;
		TArray<double> B;
	};

	struct FSolveStats
	{
		int32 Iterations = 0;
		double Residual = 0.0;
		bool bSolved = false;
	};

	struct FSolvedSignals
	{
		TArray<float> R;
		TArray<float> G;
		TArray<float> B;
		FSolveStats StatsR;
		FSolveStats StatsG;
		FSolveStats StatsB;
	};

	FORCEINLINE int32 MatIndex(int32 Size, int32 Row, int32 Col)
	{
		return Row * Size + Col;
	}

	FORCEINLINE double& At(FLinearSystemDense& System, int32 Row, int32 Col)
	{
		return System.A[MatIndex(System.Size, Row, Col)];
	}

	FORCEINLINE double AtConst(const FLinearSystemDense& System, int32 Row, int32 Col)
	{
		return System.A[MatIndex(System.Size, Row, Col)];
	}

	static bool IsObjectPath(const FString& InPath)
	{
		return InPath.Contains(TEXT("."));
	}

	static bool SaveAssetPackage(UPackage* Package, UObject* Asset)
	{
		if (!Package || !Asset)
		{
			return false;
		}

		Package->MarkPackageDirty();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Package, Asset, RF_Public | RF_Standalone, *PackageFilename, GError, nullptr, false, true, SAVE_NoError);
	}
	
	/*1. 资产汇总 (GatherAssocAssets)

	任务：它扫描你指定的目录，把所有之前生成的 UWishGIMeshAssocAsset（模型关联资产）收集起来 。


	逻辑：它建立了一个全局索引，记录哪个 Mesh 占用了 Probemap 中的哪一段探针（MeshRanges） 。*/
	static void GatherAssocAssets(const FString& AssocPath, TArray<UWishGIMeshAssocAsset*>& OutAssets)
	{
		OutAssets.Reset();

		if (IsObjectPath(AssocPath))
		{
			if (UWishGIMeshAssocAsset* SingleAsset = LoadObject<UWishGIMeshAssocAsset>(nullptr, *AssocPath))
			{
				OutAssets.Add(SingleAsset);
			}
			return;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FString> ScanPaths;
		ScanPaths.Add(AssocPath);
		AssetRegistry.ScanPathsSynchronous(ScanPaths, true);

		FARFilter Filter;
		Filter.ClassNames.Add(UWishGIMeshAssocAsset::StaticClass()->GetFName());
		Filter.PackagePaths.Add(*AssocPath);
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAssets(Filter, AssetDataList);
		for (const FAssetData& AssetData : AssetDataList)
		{
			if (UWishGIMeshAssocAsset* AssocAsset = Cast<UWishGIMeshAssocAsset>(AssetData.GetAsset()))
			{
				OutAssets.Add(AssocAsset);
			}
		}
	}

	static double HashToUnit(uint32 Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7feb352d;
		Value ^= Value >> 15;
		Value *= 0x846ca68b;
		Value ^= Value >> 16;
		return static_cast<double>(Value & 0x00FFFFFF) / 16777215.0;
	}

	static void BuildVertexTargets(uint32 MeshHash, int32 VertexIndex, double& OutR, double& OutG, double& OutB)
	{
		const uint32 SeedR = HashCombine(MeshHash, static_cast<uint32>(VertexIndex * 3 + 11));
		const uint32 SeedG = HashCombine(MeshHash, static_cast<uint32>(VertexIndex * 3 + 37));
		const uint32 SeedB = HashCombine(MeshHash, static_cast<uint32>(VertexIndex * 3 + 79));

		OutR = 0.15 + 0.85 * HashToUnit(SeedR);
		OutG = 0.15 + 0.85 * HashToUnit(SeedG);
		OutB = 0.15 + 0.85 * HashToUnit(SeedB);
	}

	static void MatVecMul(const FLinearSystemDense& System, const TArray<double>& X, TArray<double>& OutY)
	{
		OutY.Init(0.0, System.Size);
		for (int32 Row = 0; Row < System.Size; ++Row)
		{
			double Sum = 0.0;
			for (int32 Col = 0; Col < System.Size; ++Col)
			{
				Sum += AtConst(System, Row, Col) * X[Col];
			}
			OutY[Row] = Sum;
		}
	}

	static double Dot(const TArray<double>& A, const TArray<double>& B)
	{
		double Sum = 0.0;
		const int32 Num = FMath::Min(A.Num(), B.Num());
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Sum += A[Index] * B[Index];
		}
		return Sum;
	}

	// 共轭梯度求解器：用于求解 Ax=b（每个颜色通道独立求解）。
	static FSolveStats SolveByConjugateGradient(const FLinearSystemDense& System, TArray<double>& InOutX, int32 MaxIterations, double Tolerance)
	{
		FSolveStats Stats;
		if (System.Size <= 0)
		{
			return Stats;
		}

		if (InOutX.Num() != System.Size)
		{
			InOutX.Init(0.0, System.Size);
		}

		TArray<double> Ax;
		MatVecMul(System, InOutX, Ax);

		TArray<double> R;
		R.SetNum(System.Size);
		for (int32 Index = 0; Index < System.Size; ++Index)
		{
			R[Index] = System.B[Index] - Ax[Index];
		}

		TArray<double> P = R;
		double RsOld = Dot(R, R);
		Stats.Residual = FMath::Sqrt(FMath::Max(0.0, RsOld));
		if (Stats.Residual <= Tolerance)
		{
			Stats.bSolved = true;
			return Stats;
		}

		TArray<double> Ap;
		for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
		{
			MatVecMul(System, P, Ap);
			const double Denom = Dot(P, Ap);
			if (FMath::Abs(Denom) < 1e-20)
			{
				break;
			}

			const double Alpha = RsOld / Denom;
			for (int32 Index = 0; Index < System.Size; ++Index)
			{
				InOutX[Index] += Alpha * P[Index];
				R[Index] -= Alpha * Ap[Index];
			}

			const double RsNew = Dot(R, R);
			Stats.Iterations = Iteration + 1;
			Stats.Residual = FMath::Sqrt(FMath::Max(0.0, RsNew));
			if (Stats.Residual <= Tolerance)
			{
				Stats.bSolved = true;
				return Stats;
			}

			const double Beta = RsNew / FMath::Max(RsOld, 1e-30);
			for (int32 Index = 0; Index < System.Size; ++Index)
			{
				P[Index] = R[Index] + (Beta * P[Index]);
			}

			RsOld = RsNew;
		}

		Stats.bSolved = (Stats.Residual <= Tolerance * 4.0);
		return Stats;
	}

	// 构建法方程系统：数据项 + Lambda 正则项。
	static void BuildSystems(
		const UWishGIMeshAssocAsset* AssocAsset,
		int32 MeshProbeCount,
		float Lambda,
		FLinearSystemDense& OutSystemR,
		FLinearSystemDense& OutSystemG,
		FLinearSystemDense& OutSystemB)
	{
		OutSystemR.Size = MeshProbeCount;
		OutSystemG.Size = MeshProbeCount;
		OutSystemB.Size = MeshProbeCount;

		const int32 MatSize = MeshProbeCount * MeshProbeCount;
		OutSystemR.A.Init(0.0, MatSize);
		OutSystemG.A.Init(0.0, MatSize);
		OutSystemB.A.Init(0.0, MatSize);
		OutSystemR.B.Init(0.0, MeshProbeCount);
		OutSystemG.B.Init(0.0, MeshProbeCount);
		OutSystemB.B.Init(0.0, MeshProbeCount);

		const uint32 MeshHash = GetTypeHash(AssocAsset->GetPathName());
		const int32 VertexCount = AssocAsset->VertexAssociations.Num();
		if (VertexCount <= 0)
		{
			for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
			{
				const double Uniform = 1.0 / static_cast<double>(MeshProbeCount);
				OutSystemR.B[ProbeIndex] = Uniform;
				OutSystemG.B[ProbeIndex] = Uniform;
				OutSystemB.B[ProbeIndex] = Uniform;
				At(OutSystemR, ProbeIndex, ProbeIndex) = 1.0;
				At(OutSystemG, ProbeIndex, ProbeIndex) = 1.0;
				At(OutSystemB, ProbeIndex, ProbeIndex) = 1.0;
			}
			return;
		}

		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			const FWishGIProbeVertexAssociation& Assoc = AssocAsset->VertexAssociations[VertexIndex];

			int32 P0 = FMath::Clamp<int32>(Assoc.ProbeIndex0, 0, MeshProbeCount - 1);
			int32 P1 = FMath::Clamp<int32>(Assoc.ProbeIndex1, 0, MeshProbeCount - 1);
			double W0 = static_cast<double>(Assoc.Weight0) / 255.0;
			double W1 = static_cast<double>(Assoc.Weight1) / 255.0;

			if (P0 == P1 || W1 <= 1e-6)
			{
				P1 = P0;
				W0 = 1.0;
				W1 = 0.0;
			}
			else
			{
				const double Norm = FMath::Max(W0 + W1, 1e-9);
				W0 /= Norm;
				W1 /= Norm;
			}

			double TargetR = 0.0;
			double TargetG = 0.0;
			double TargetB = 0.0;
			BuildVertexTargets(MeshHash, VertexIndex, TargetR, TargetG, TargetB);

			At(OutSystemR, P0, P0) += W0 * W0;
			At(OutSystemR, P0, P1) += W0 * W1;
			At(OutSystemR, P1, P0) += W1 * W0;
			At(OutSystemR, P1, P1) += W1 * W1;

			OutSystemR.B[P0] += W0 * TargetR;
			OutSystemR.B[P1] += W1 * TargetR;

			At(OutSystemG, P0, P0) += W0 * W0;
			At(OutSystemG, P0, P1) += W0 * W1;
			At(OutSystemG, P1, P0) += W1 * W0;
			At(OutSystemG, P1, P1) += W1 * W1;

			OutSystemG.B[P0] += W0 * TargetG;
			OutSystemG.B[P1] += W1 * TargetG;

			At(OutSystemB, P0, P0) += W0 * W0;
			At(OutSystemB, P0, P1) += W0 * W1;
			At(OutSystemB, P1, P0) += W1 * W0;
			At(OutSystemB, P1, P1) += W1 * W1;

			OutSystemB.B[P0] += W0 * TargetB;
			OutSystemB.B[P1] += W1 * TargetB;

			if (P0 != P1)
			{
				const double EdgeReg = static_cast<double>(Lambda) * (0.1 + 0.5 * (W0 + W1));

				At(OutSystemR, P0, P0) += EdgeReg;
				At(OutSystemR, P1, P1) += EdgeReg;
				At(OutSystemR, P0, P1) -= EdgeReg;
				At(OutSystemR, P1, P0) -= EdgeReg;

				At(OutSystemG, P0, P0) += EdgeReg;
				At(OutSystemG, P1, P1) += EdgeReg;
				At(OutSystemG, P0, P1) -= EdgeReg;
				At(OutSystemG, P1, P0) -= EdgeReg;

				At(OutSystemB, P0, P0) += EdgeReg;
				At(OutSystemB, P1, P1) += EdgeReg;
				At(OutSystemB, P0, P1) -= EdgeReg;
				At(OutSystemB, P1, P0) -= EdgeReg;
			}
		}

		const double DiagEpsilon = 1e-4 + static_cast<double>(Lambda) * 1e-3;
		for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
		{
			At(OutSystemR, ProbeIndex, ProbeIndex) += DiagEpsilon;
			At(OutSystemG, ProbeIndex, ProbeIndex) += DiagEpsilon;
			At(OutSystemB, ProbeIndex, ProbeIndex) += DiagEpsilon;
		}
	}

	// 单个 Mesh 的 probe 信号求解入口。
	static FSolvedSignals SolveProbeSignals(const UWishGIMeshAssocAsset* AssocAsset, int32 MeshProbeCount, float Lambda)
	{
		FSolvedSignals Result;
		if (!AssocAsset || MeshProbeCount <= 0)
		{
			return Result;
		}

		FLinearSystemDense SystemR;
		FLinearSystemDense SystemG;
		FLinearSystemDense SystemB;
		BuildSystems(AssocAsset, MeshProbeCount, Lambda, SystemR, SystemG, SystemB);

		const int32 MaxIterations = FMath::Clamp(MeshProbeCount * 4, 32, 512);
		const double Tolerance = 1e-5;

		TArray<double> XR;
		TArray<double> XG;
		TArray<double> XB;

		Result.StatsR = SolveByConjugateGradient(SystemR, XR, MaxIterations, Tolerance);
		Result.StatsG = SolveByConjugateGradient(SystemG, XG, MaxIterations, Tolerance);
		Result.StatsB = SolveByConjugateGradient(SystemB, XB, MaxIterations, Tolerance);

		Result.R.SetNum(MeshProbeCount);
		Result.G.SetNum(MeshProbeCount);
		Result.B.SetNum(MeshProbeCount);

		for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
		{
			const double RV = XR.IsValidIndex(ProbeIndex) ? XR[ProbeIndex] : 0.0;
			const double GV = XG.IsValidIndex(ProbeIndex) ? XG[ProbeIndex] : 0.0;
			const double BV = XB.IsValidIndex(ProbeIndex) ? XB[ProbeIndex] : 0.0;
			Result.R[ProbeIndex] = static_cast<float>(FMath::Clamp(RV, 0.0, 1.0));
			Result.G[ProbeIndex] = static_cast<float>(FMath::Clamp(GV, 0.0, 1.0));
			Result.B[ProbeIndex] = static_cast<float>(FMath::Clamp(BV, 0.0, 1.0));
		}

		return Result;
	}

	static FLinearColor BuildTint(uint32 Hash)
	{
		const float R = 0.75f + 0.25f * static_cast<float>(HashToUnit(HashCombine(Hash, 17u)));
		const float G = 0.75f + 0.25f * static_cast<float>(HashToUnit(HashCombine(Hash, 29u)));
		const float B = 0.75f + 0.25f * static_cast<float>(HashToUnit(HashCombine(Hash, 43u)));
		return FLinearColor(R, G, B, 1.0f);
	}

	/*
	1. 输入覆盖率 Coverage、SHOrder、Lambda、mesh hash。
	2. 生成稳定 tint（基于 hash，保证可复现）。
	3. Pixel0 用覆盖率调制亮度。
	4. 若三阶 SH，再生成 Pixel1。
	
	注意：这还是“占位烘焙记录”，不是论文里的真实光照拟合解。
	
	3. 探针记录生成 (BuildProbeRecord)
	数据结构：


	Pixel 0：存储 1 阶和 2 阶球谐系数（4 个 10-bit 数据存入一个像素） 。


	Pixel 1：如果开启了 3 阶（SHOrder=3），则占用第二个像素存储剩余系数 。

	MVP 伪数据：注意代码中的 BuildTint 和 BuildProbeRecord。目前它们生成的不是真实光照，而是根据 Mesh 名字生成的调试颜色（Tint）。

	验证意义：如果你在渲染时看到物体被染成了彩色的色块，且色块边界平滑，说明你的“顶点-探针”查找逻辑是 100% 正确的。
	*/
	static FWishGIProbeSHRecord BuildProbeRecord(float Coverage, int32 SHOrder, uint32 MeshHash, int32 ProbeIndex, float Lambda)
	{
		const FLinearColor Tint = BuildTint(HashCombine(MeshHash, static_cast<uint32>(ProbeIndex + 1)));

		const float R = FMath::Clamp(SignalR * Tint.R, 0.0f, 1.0f);
		const float G = FMath::Clamp(SignalG * Tint.G, 0.0f, 1.0f);
		const float B = FMath::Clamp(SignalB * Tint.B, 0.0f, 1.0f);
		const float Base = (R + G + B) / 3.0f;

		FWishGIProbeSHRecord Record;
		Record.Pixel0 = FLinearColor(R, G, B, 1.0f);

		if (SHOrder >= 3)
		{
			const float Detail = (FMath::Abs(R - Base) + FMath::Abs(G - Base) + FMath::Abs(B - Base)) / 3.0f;
			Record.Pixel1 = FLinearColor(Base, Detail, 1.0f - Base, 1.0f);
		}

		return Record;
	}

	static FIntPoint ComputeProbeMapSize(int32 TotalProbes, int32 SHOrder)
	{
		const int32 PixelsPerProbe = (SHOrder >= 3) ? 2 : 1;
		const int32 TotalPixels = FMath::Max(1, TotalProbes * PixelsPerProbe);
		const uint32 WidthPow2 = FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(TotalPixels))))));
		const int32 Width = static_cast<int32>(WidthPow2);
		const int32 Rows = FMath::Max(1, FMath::DivideAndRoundUp(TotalPixels, Width));
		const uint32 HeightPow2 = FMath::RoundUpToPowerOfTwo(static_cast<uint32>(Rows));
		const int32 Height = static_cast<int32>(HeightPow2);
		return FIntPoint(Width, Height);
	}
}

UWishGIBakeSceneCommandlet::UWishGIBakeSceneCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

void UWishGIBakeSceneCommandlet::PrintUsage() const
{
	UE_LOG(LogWishGIBakeScene, Display, TEXT("WishGI BakeScene Commandlet Usage:"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -run=WishGIBakeScene"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Map=/Game/Maps/YourMap (optional for MVP metadata)"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -AssocPath=/Game/WishGI/MeshAssoc"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -OutPath=/Game/WishGI/Bake"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -AssetName=WishGI_ProbeMap"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -SHOrder=2"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Directions=192"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Lambda=0.1"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Overwrite"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Help"));
}

// 命令主流程：解析参数 -> 收集输入资产 -> 求解 -> 写入 ProbeMap 资产。
int32 UWishGIBakeSceneCommandlet::Main(const FString& Params)
{
	WishGIBakeScene::FSettings Settings;
	FParse::Value(*Params, TEXT("Map="), Settings.MapPath);
	FParse::Value(*Params, TEXT("AssocPath="), Settings.AssocPath);
	FParse::Value(*Params, TEXT("OutPath="), Settings.OutPath);
	FParse::Value(*Params, TEXT("AssetName="), Settings.AssetName);
	FParse::Value(*Params, TEXT("SHOrder="), Settings.SHOrder);
	FParse::Value(*Params, TEXT("Directions="), Settings.Directions);
	FParse::Value(*Params, TEXT("Lambda="), Settings.Lambda);
	Settings.bOverwrite = FParse::Param(*Params, TEXT("Overwrite"));
	Settings.bHelp = FParse::Param(*Params, TEXT("Help")) || FParse::Param(*Params, TEXT("?"));

	if (Settings.bHelp)
	{
		PrintUsage();
		return 0;
	}

	if (!FPackageName::IsValidLongPackageName(Settings.OutPath))
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Invalid OutPath '%s'."), *Settings.OutPath);
		return 1;
	}

	if (Settings.AssetName.IsEmpty())
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("AssetName cannot be empty."));
		return 1;
	}

	Settings.SHOrder = FMath::Clamp(Settings.SHOrder, 2, 3);
	Settings.Directions = FMath::Max(1, Settings.Directions);
	Settings.Lambda = FMath::Clamp(Settings.Lambda, 0.0f, 10.0f);

	TArray<UWishGIMeshAssocAsset*> AssocAssets;
	WishGIBakeScene::GatherAssocAssets(Settings.AssocPath, AssocAssets);
	if (AssocAssets.Num() == 0)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("No mesh association assets found at '%s'."), *Settings.AssocPath);
		return 2;
	}

	//读取 AssocPath 下所有 UWishGIMeshAssocAsset。
	FString TrimmedOutPath = Settings.OutPath;
	TrimmedOutPath.RemoveFromEnd(TEXT("/"));
	const FString PackageName = FString::Printf(TEXT("%s/%s"), *TrimmedOutPath, *Settings.AssetName);
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Settings.AssetName);

	bool bIsNewAsset = false;
	UWishGIProbeMapAsset* ProbeMapAsset = LoadObject<UWishGIProbeMapAsset>(nullptr, *ObjectPath);
	if (!ProbeMapAsset)
	{
		UPackage* Package = CreatePackage(*PackageName);
		ProbeMapAsset = NewObject<UWishGIProbeMapAsset>(Package, *Settings.AssetName, RF_Public | RF_Standalone);
		bIsNewAsset = true;
	}
	else if (!Settings.bOverwrite)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Asset '%s' already exists. Use -Overwrite to replace."), *ObjectPath);
		return 3;
	}

	ProbeMapAsset->SourceMapPath = Settings.MapPath;
	ProbeMapAsset->SHOrder = Settings.SHOrder;
	ProbeMapAsset->DirectionCount = Settings.Directions;
	ProbeMapAsset->Lambda = Settings.Lambda;
	ProbeMapAsset->ProbeMapTexture.Reset();
	ProbeMapAsset->ProbeRecords.Reset();
	ProbeMapAsset->MeshRanges.Reset();
	ProbeMapAsset->SolverIterations = 0;
	ProbeMapAsset->SolverResidual = 0.0f;

	/*
	 每个 mesh：
	6. 记 ProbeStart/ProbeCount 到 MeshRanges。
	7. 算该 mesh 的 coverage。
	8. 生成每个 probe 的 SH 占位记录。
	9. 汇总出 TotalProbeCount。
	10. 用 ComputeProbeMapSize 算建议纹理尺寸。
	11. 保存为 UWishGIProbeMapAsset。
	*/
	int32 RunningProbeStart = 0;
	double ResidualAccum = 0.0;
	int32 IterationAccum = 0;
	int32 SolvedChannelCount = 0;

	for (UWishGIMeshAssocAsset* AssocAsset : AssocAssets)
	{
		if (!AssocAsset)
		{
			continue;
		}

		const int32 MeshProbeCount = FMath::Clamp(AssocAsset->ProbeCount, 1, 256);

		FWishGIProbeMeshRange MeshRange;
		MeshRange.SourceMesh = AssocAsset->SourceMesh;
		MeshRange.ProbeStart = RunningProbeStart;
		MeshRange.ProbeCount = MeshProbeCount;
		ProbeMapAsset->MeshRanges.Add(MeshRange);

		const WishGIBakeScene::FSolvedSignals Signals = WishGIBakeScene::SolveProbeSignals(AssocAsset, MeshProbeCount, Settings.Lambda);
		const uint32 MeshHash = GetTypeHash(AssocAsset->GetPathName());

		auto AccStats = [&ResidualAccum, &IterationAccum, &SolvedChannelCount](const WishGIBakeScene::FSolveStats& Stats)
		{
			if (Stats.Iterations > 0)
			{
				IterationAccum += Stats.Iterations;
				ResidualAccum += Stats.Residual;
				SolvedChannelCount += 1;
			}
		};

		AccStats(Signals.StatsR);
		AccStats(Signals.StatsG);
		AccStats(Signals.StatsB);

		for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
		{
			const float R = Signals.R.IsValidIndex(ProbeIndex) ? Signals.R[ProbeIndex] : 0.0f;
			const float G = Signals.G.IsValidIndex(ProbeIndex) ? Signals.G[ProbeIndex] : 0.0f;
			const float B = Signals.B.IsValidIndex(ProbeIndex) ? Signals.B[ProbeIndex] : 0.0f;
			ProbeMapAsset->ProbeRecords.Add(WishGIBakeScene::BuildProbeRecord(R, G, B, Settings.SHOrder, MeshHash, ProbeIndex));
		}

		RunningProbeStart += MeshProbeCount;
	}

	ProbeMapAsset->TotalProbeCount = RunningProbeStart;
	ProbeMapAsset->SuggestedProbeMapSize = WishGIBakeScene::ComputeProbeMapSize(ProbeMapAsset->TotalProbeCount, Settings.SHOrder);

	if (SolvedChannelCount > 0)
	{
		ProbeMapAsset->SolverIterations = FMath::RoundToInt(static_cast<float>(IterationAccum) / static_cast<float>(SolvedChannelCount));
		ProbeMapAsset->SolverResidual = static_cast<float>(ResidualAccum / static_cast<double>(SolvedChannelCount));
	}

	if (bIsNewAsset)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.AssetCreated(ProbeMapAsset);
	}

	if (!WishGIBakeScene::SaveAssetPackage(ProbeMapAsset->GetOutermost(), ProbeMapAsset))
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Failed to save '%s'."), *ObjectPath);
		return 4;
	}

	UE_LOG(LogWishGIBakeScene, Display, TEXT("Bake complete: '%s'"), *ObjectPath);
	UE_LOG(LogWishGIBakeScene, Display, TEXT("AssocAssets=%d, TotalProbes=%d, SHOrder=%d, SuggestedProbeMap=%dx%d, AvgSolverIter=%d, AvgResidual=%.6f"),
		AssocAssets.Num(),
		ProbeMapAsset->TotalProbeCount,
		ProbeMapAsset->SHOrder,
		ProbeMapAsset->SuggestedProbeMapSize.X,
		ProbeMapAsset->SuggestedProbeMapSize.Y,
		ProbeMapAsset->SolverIterations,
		ProbeMapAsset->SolverResidual);
	return 0;
}

