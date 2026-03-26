#include "Commandlets/WishGIMeshPrepCommandlet.h"

#include "AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Engine/StaticMesh.h"
#include "Math/RandomStream.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#include "WishGIMeshAssocAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogWishGIMeshPrep, Log, All);

namespace WishGIMeshPrep
{
	// 中文说明：
	// 1) 本文件负责 Mesh 预处理：从静态网格生成每顶点的 probe 索引与权重。
	// 2) 核心步骤：读取三角形 -> 面积加权采样 -> probe 中心聚类 -> 权重回传顶点。
	// 3) 产物是 UWishGIMeshAssocAsset，供 Bake 阶段与运行时重建使用。
	struct FSettings
	{
		FString MeshPath = TEXT("/Game");
		FString OutPath = TEXT("/Game/WishGI/MeshAssoc");
		float SampleDensity = 100.0f;
		int32 ProbeCount = 32;
		int32 AssociationsPerVertex = 2;
		int32 LODIndex = 0;
		int32 KMeansIterations = 8;
		int32 Seed = 1337;
		int32 MinSamples = 0;
		int32 MaxSamples = 200000;
		bool bOverwrite = false;
		bool bHelp = false;
	};

	struct FTriangleData
	{
		int32 V0 = INDEX_NONE;
		int32 V1 = INDEX_NONE;
		int32 V2 = INDEX_NONE;
		FVector P0 = FVector::ZeroVector;
		FVector P1 = FVector::ZeroVector;
		FVector P2 = FVector::ZeroVector;
		float Area = 0.0f;
		float CumulativeArea = 0.0f;
	};

	struct FSamplePoint
	{
		int32 TriangleIndex = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
		FVector Bary = FVector::ZeroVector;
		int32 Probe0 = 0;
		int32 Probe1 = 0;
		float ProbeWeight0 = 1.0f;
		float ProbeWeight1 = 0.0f;
	};

	static bool IsObjectPath(const FString& InPath)
	{
		return InPath.Contains(TEXT("."));
	}
	
	/*5. 资源序列化 (SaveAssetPackage)
	任务：把算好的这些探针索引、权重、顶点关联数据，打包存成一个自定义的 UE 资产：UWishGIMeshAssocAsset。

	价值：这样在渲染运行时，引擎直接读取这个资产即可，不需要再做任何复杂的几何计算。*/
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

	static void GatherMeshes(const FString& MeshPath, TArray<UStaticMesh*>& OutMeshes)
	{
		OutMeshes.Reset();

		if (IsObjectPath(MeshPath))
		{
			if (UStaticMesh* SingleMesh = LoadObject<UStaticMesh>(nullptr, *MeshPath))
			{
				OutMeshes.Add(SingleMesh);
			}
			return;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FString> ScanPaths;
		ScanPaths.Add(MeshPath);
		AssetRegistry.ScanPathsSynchronous(ScanPaths, true);

		FARFilter Filter;
		Filter.ClassNames.Add(UStaticMesh::StaticClass()->GetFName());
		Filter.PackagePaths.Add(*MeshPath);
		Filter.bRecursivePaths = true;

		TArray<FAssetData> MeshAssets;
		AssetRegistry.GetAssets(Filter, MeshAssets);
		for (const FAssetData& AssetData : MeshAssets)
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				OutMeshes.Add(Mesh);
			}
		}
	}

	static FString MakeAssetPackageName(const FString& OutPath, const UStaticMesh* Mesh)
	{
		FString TrimmedOutPath = OutPath;
		TrimmedOutPath.RemoveFromEnd(TEXT("/"));

		const uint32 Hash = GetTypeHash(Mesh->GetPathName());
		const FString AssetName = FString::Printf(TEXT("WGA_%s_%08X"), *Mesh->GetName(), Hash);
		return FString::Printf(TEXT("%s/%s"), *TrimmedOutPath, *AssetName);
	}
	
	/*1. 几何体提取 (BuildTriangles)
	任务：从 UE4 的 StaticMeshRenderData 中抓取三角形数据。

	关键点：它计算了每个三角形的面积（通过叉积），并建立了一个“累积面积表”。这为下一步的均匀采样做准备。*/
	// 提取 LOD 三角形与面积信息，为后续面积加权采样做准备。
	static bool BuildTriangles(const UStaticMesh* Mesh, int32 LODIndex, int32& OutVertexCount, TArray<FTriangleData>& OutTriangles, float& OutTotalArea)
	{
		OutVertexCount = 0;
		OutTriangles.Reset();
		OutTotalArea = 0.0f;

		if (!Mesh)
		{
			return false;
		}

		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(LODIndex))
		{
			return false;
		}

		const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
		const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
		const FRawStaticIndexBuffer& IndexBuffer = LOD.IndexBuffer;

		const int32 VertexCount = PositionBuffer.GetNumVertices();
		const int32 NumIndices = IndexBuffer.GetNumIndices();
		if (VertexCount <= 0 || NumIndices < 3)
		{
			return false;
		}

		OutVertexCount = VertexCount;
		OutTriangles.Reserve(NumIndices / 3);

		for (int32 Index = 0; Index + 2 < NumIndices; Index += 3)
		{
			const int32 I0 = static_cast<int32>(IndexBuffer.GetIndex(static_cast<uint32>(Index)));
			const int32 I1 = static_cast<int32>(IndexBuffer.GetIndex(static_cast<uint32>(Index + 1)));
			const int32 I2 = static_cast<int32>(IndexBuffer.GetIndex(static_cast<uint32>(Index + 2)));

			if (I0 < 0 || I1 < 0 || I2 < 0 || I0 >= VertexCount || I1 >= VertexCount || I2 >= VertexCount)
			{
				continue;
			}

			const FVector P0 = PositionBuffer.VertexPosition(I0);
			const FVector P1 = PositionBuffer.VertexPosition(I1);
			const FVector P2 = PositionBuffer.VertexPosition(I2);

			const float Area = 0.5f * FVector::CrossProduct(P1 - P0, P2 - P0).Size();
			if (Area <= KINDA_SMALL_NUMBER)
			{
				continue;
			}

			FTriangleData Tri;
			Tri.V0 = I0;
			Tri.V1 = I1;
			Tri.V2 = I2;
			Tri.P0 = P0;
			Tri.P1 = P1;
			Tri.P2 = P2;
			Tri.Area = Area;
			OutTotalArea += Area;
			Tri.CumulativeArea = OutTotalArea;
			OutTriangles.Add(Tri);
		}

		return OutTriangles.Num() > 0 && OutTotalArea > KINDA_SMALL_NUMBER;
	}

	static int32 FindTriangleByArea(const TArray<FTriangleData>& Triangles, float AreaValue)
	{
		int32 Low = 0;
		int32 High = Triangles.Num() - 1;
		while (Low < High)
		{
			const int32 Mid = (Low + High) / 2;
			if (Triangles[Mid].CumulativeArea < AreaValue)
			{
				Low = Mid + 1;
			}
			else
			{
				High = Mid;
			}
		}
		return Low;
	}

	/*
	1. 先按三角形累计面积做“概率分布”。
	2. 每次随机一个面积值，二分找到对应三角形。
	3. 在该三角形内部用重心坐标随机一点。
	4. 产出 Sample.Position 和 Sample.Bary。

	意义：采样密度跟表面积成比例，避免小三角被过采样/大三角被欠采样。

	2. 表面采样 (GenerateSurfaceSamples)
	任务：在模型表面撒下数万个采样点。

	数学实现：使用了重心坐标采样法。它先根据三角形面积加权随机选一个三角形，然后在三角形内随机生成一个点。

	目的：确保采样点在模型表面是物理均匀的（面积大的地方点多，小的地方点少），这符合 WishGI 论文中对表面覆盖的要求。
	*/
	// 按三角形面积随机采样模型表面点。
	static void GenerateSurfaceSamples(const TArray<FTriangleData>& Triangles, float TotalArea, int32 SampleCount, int32 Seed, TArray<FSamplePoint>& OutSamples)
	{
		OutSamples.Reset();
		OutSamples.Reserve(SampleCount);

		FRandomStream RNG(Seed);
		for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
		{
			const float RArea = RNG.FRandRange(0.0f, TotalArea);
			const int32 TriangleIndex = FindTriangleByArea(Triangles, RArea);
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const FTriangleData& Tri = Triangles[TriangleIndex];
			const float U = RNG.FRand();
			const float V = RNG.FRand();
			const float SU = FMath::Sqrt(U);
			const float B0 = 1.0f - SU;
			const float B1 = SU * (1.0f - V);
			const float B2 = SU * V;

			FSamplePoint Sample;
			Sample.TriangleIndex = TriangleIndex;
			Sample.Bary = FVector(B0, B1, B2);
			Sample.Position = (Tri.P0 * B0) + (Tri.P1 * B1) + (Tri.P2 * B2);
			OutSamples.Add(Sample);
		}
	}
	
	/*
	1. 从采样点里均匀挑初始中心。
	2. 迭代：
	3. 把每个采样点分到最近中心。
	4. 每簇算质心。
	5. 在簇内找离质心最近的真实采样点作为新中心（更接近 k-medoids 味道）。
	
	意义：得到一组稳定 probe 中心，供后续关联。
  
	3. 探针聚类 (ComputeProbeCenters)
	任务：将成千上万个采样点，“浓缩”成几十个（代码默认 32 个）探针（Probes）。

	算法：实现了 K-Means 聚类（代码中其实是 K-Medoids 的一种变体）。

	逻辑：它不断迭代，寻找能代表这一片采样点的“中心位置”。最终，这些中心点就是该物体的局部 GI 探针。
	*/
	// 通过 KMeans/KMedoids-like 迭代初始化 probe 中心。
	static void ComputeProbeCenters(const TArray<FSamplePoint>& Samples, int32 ProbeCount, int32 Iterations, TArray<FVector>& OutCenters)
	{
		OutCenters.Reset();
		const int32 K = FMath::Clamp(ProbeCount, 1, Samples.Num());
		OutCenters.Reserve(K);

		for (int32 CenterIndex = 0; CenterIndex < K; ++CenterIndex)
		{
			const int32 SampleIndex = (CenterIndex * Samples.Num()) / K;
			OutCenters.Add(Samples[SampleIndex].Position);
		}

		if (K <= 1 || Iterations <= 0)
		{
			return;
		}

		TArray<int32> Assignments;
		Assignments.Init(0, Samples.Num());

		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			TArray<FVector> Sums;
			TArray<int32> Counts;
			Sums.Init(FVector::ZeroVector, K);
			Counts.Init(0, K);

			for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
			{
				float BestDist = TNumericLimits<float>::Max();
				int32 BestCenter = 0;
				for (int32 CenterIndex = 0; CenterIndex < K; ++CenterIndex)
				{
					const float Dist = FVector::DistSquared(Samples[SampleIndex].Position, OutCenters[CenterIndex]);
					if (Dist < BestDist)
					{
						BestDist = Dist;
						BestCenter = CenterIndex;
					}
				}

				Assignments[SampleIndex] = BestCenter;
				Sums[BestCenter] += Samples[SampleIndex].Position;
				Counts[BestCenter] += 1;
			}

			for (int32 CenterIndex = 0; CenterIndex < K; ++CenterIndex)
			{
				if (Counts[CenterIndex] <= 0)
				{
					continue;
				}

				const FVector Centroid = Sums[CenterIndex] / static_cast<float>(Counts[CenterIndex]);
				float BestDist = TNumericLimits<float>::Max();
				int32 BestSample = INDEX_NONE;

				for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
				{
					if (Assignments[SampleIndex] != CenterIndex)
					{
						continue;
					}

					const float Dist = FVector::DistSquared(Samples[SampleIndex].Position, Centroid);
					if (Dist < BestDist)
					{
						BestDist = Dist;
						BestSample = SampleIndex;
					}
				}

				if (BestSample != INDEX_NONE)
				{
					OutCenters[CenterIndex] = Samples[BestSample].Position;
				}
			}
		}
	}

	/*4. 建立“顶点-探针”关联 (BuildVertexAssociations)
	这是 WishGI 的灵魂步骤：

	任务：计算模型上的每一个顶点（Vertex）应该受哪两个探针的影响，比例是多少。

	权重算法：采用反距离加权（Inverse Distance Weighting）。离探针越近的顶点，权重越高。

	数据压缩：它将权重归一化并量化为 uint8（0-255），这意味着在渲染时，每个顶点只需要存储极少的数据。*/
	static void ComputeSampleAssociations(TArray<FSamplePoint>& Samples, const TArray<FVector>& Centers, int32 AssociationsPerSample)
	{
		if (Centers.Num() == 0)
		{
			return;
		}

		for (FSamplePoint& Sample : Samples)
		{
			float Best0 = TNumericLimits<float>::Max();
			float Best1 = TNumericLimits<float>::Max();
			int32 Probe0 = 0;
			int32 Probe1 = 0;

			for (int32 CenterIndex = 0; CenterIndex < Centers.Num(); ++CenterIndex)
			{
				const float Dist = FVector::DistSquared(Sample.Position, Centers[CenterIndex]);
				if (Dist < Best0)
				{
					Best1 = Best0;
					Probe1 = Probe0;
					Best0 = Dist;
					Probe0 = CenterIndex;
				}
				else if (Dist < Best1)
				{
					Best1 = Dist;
					Probe1 = CenterIndex;
				}
			}

			if (Centers.Num() == 1)
			{
				Probe1 = Probe0;
				Best1 = Best0;
			}

			const float D0 = FMath::Max(FMath::Sqrt(Best0), 1e-3f);
			const float D1 = FMath::Max(FMath::Sqrt(Best1), 1e-3f);
			const float Inv0 = 1.0f / D0;
			const float Inv1 = (AssociationsPerSample > 1) ? (1.0f / D1) : 0.0f;
			const float InvSum = FMath::Max(Inv0 + Inv1, 1e-6f);

			Sample.Probe0 = Probe0;
			Sample.Probe1 = Probe1;
			Sample.ProbeWeight0 = Inv0 / InvSum;
			Sample.ProbeWeight1 = (AssociationsPerSample > 1) ? (Inv1 / InvSum) : 0.0f;
		}
	}

	/*
	它做什么：

	1. 对每个采样点，把它对 probe 的权重按重心坐标分摊到三角形 3 个顶点。
	2. 每个顶点会累计一堆 (probe -> weight)。
	3. 取权重最大的前 2 个 probe。
	4. 归一化并量化成 uint8（Weight0/Weight1）。

	意义：把“采样点级信息”压缩成“每顶点 2 个 probe”，对运行时成本友好。
	*/
	// 将采样点的 probe 权重通过重心坐标累积到顶点，并保留 top-2。
	static void BuildVertexAssociations(
		int32 VertexCount,
		const TArray<FTriangleData>& Triangles,
		const TArray<FSamplePoint>& Samples,
		int32 ProbeCount,
		int32 AssociationsPerVertex,
		TArray<FWishGIProbeVertexAssociation>& OutAssociations)
	{
		OutAssociations.Reset();
		OutAssociations.SetNum(VertexCount);

		TArray<TMap<int32, float>> VertexProbeWeights;
		VertexProbeWeights.SetNum(VertexCount);

		auto AddContribution = [&VertexProbeWeights, VertexCount](int32 VertexIndex, int32 ProbeIndex, float Value)
		{
			if (VertexIndex < 0 || VertexIndex >= VertexCount || ProbeIndex < 0 || Value <= 0.0f)
			{
				return;
			}
			VertexProbeWeights[VertexIndex].FindOrAdd(ProbeIndex) += Value;
		};

		for (const FSamplePoint& Sample : Samples)
		{
			if (!Triangles.IsValidIndex(Sample.TriangleIndex))
			{
				continue;
			}

			const FTriangleData& Tri = Triangles[Sample.TriangleIndex];
			const float B0 = Sample.Bary.X;
			const float B1 = Sample.Bary.Y;
			const float B2 = Sample.Bary.Z;

			AddContribution(Tri.V0, Sample.Probe0, B0 * Sample.ProbeWeight0);
			AddContribution(Tri.V1, Sample.Probe0, B1 * Sample.ProbeWeight0);
			AddContribution(Tri.V2, Sample.Probe0, B2 * Sample.ProbeWeight0);

			if (AssociationsPerVertex > 1)
			{
				AddContribution(Tri.V0, Sample.Probe1, B0 * Sample.ProbeWeight1);
				AddContribution(Tri.V1, Sample.Probe1, B1 * Sample.ProbeWeight1);
				AddContribution(Tri.V2, Sample.Probe1, B2 * Sample.ProbeWeight1);
			}
		}

		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			FWishGIProbeVertexAssociation Entry;
			const TMap<int32, float>& Weights = VertexProbeWeights[VertexIndex];

			if (Weights.Num() == 0)
			{
				Entry.ProbeIndex0 = 0;
				Entry.ProbeIndex1 = 0;
				Entry.Weight0 = 255;
				Entry.Weight1 = 0;
				OutAssociations[VertexIndex] = Entry;
				continue;
			}

			TArray<TPair<int32, float>> Sorted;
			Sorted.Reserve(Weights.Num());
			float SumWeights = 0.0f;
			for (const TPair<int32, float>& Pair : Weights)
			{
				if (Pair.Value > 0.0f)
				{
					Sorted.Add(Pair);
					SumWeights += Pair.Value;
				}
			}

			if (Sorted.Num() == 0)
			{
				Entry.ProbeIndex0 = 0;
				Entry.ProbeIndex1 = 0;
				Entry.Weight0 = 255;
				Entry.Weight1 = 0;
				OutAssociations[VertexIndex] = Entry;
				continue;
			}

			if (SumWeights > 1e-6f)
			{
				for (TPair<int32, float>& Pair : Sorted)
				{
					Pair.Value /= SumWeights;
				}
			}

			Sorted.Sort([](const TPair<int32, float>& A, const TPair<int32, float>& B)
			{
				return A.Value > B.Value;
			});

			const int32 P0 = FMath::Clamp(Sorted[0].Key, 0, FMath::Max(ProbeCount - 1, 0));
			Entry.ProbeIndex0 = static_cast<uint8>(FMath::Clamp(P0, 0, 255));

			if (AssociationsPerVertex <= 1 || Sorted.Num() < 2)
			{
				Entry.ProbeIndex1 = Entry.ProbeIndex0;
				Entry.Weight0 = 255;
				Entry.Weight1 = 0;
				OutAssociations[VertexIndex] = Entry;
				continue;
			}

			const int32 P1 = FMath::Clamp(Sorted[1].Key, 0, FMath::Max(ProbeCount - 1, 0));
			Entry.ProbeIndex1 = static_cast<uint8>(FMath::Clamp(P1, 0, 255));

			const float TopSum = FMath::Max(Sorted[0].Value + Sorted[1].Value, 1e-6f);
			const float W0 = FMath::Clamp(Sorted[0].Value / TopSum, 0.0f, 1.0f);
			const int32 Q0 = FMath::Clamp(FMath::RoundToInt(W0 * 255.0f), 0, 255);
			Entry.Weight0 = static_cast<uint8>(Q0);
			Entry.Weight1 = static_cast<uint8>(255 - Q0);

			OutAssociations[VertexIndex] = Entry;
		}
	}

	static int32 ComputeSampleCount(float TotalAreaCm2, const FSettings& Settings)
	{
		const float AreaM2 = TotalAreaCm2 / 10000.0f;
		int32 SampleCount = FMath::CeilToInt(AreaM2 * Settings.SampleDensity);
		SampleCount = FMath::Max(SampleCount, FMath::Max(1, Settings.ProbeCount * 2));
		if (Settings.MinSamples > 0)
		{
			SampleCount = FMath::Max(SampleCount, Settings.MinSamples);
		}
		if (Settings.MaxSamples > 0)
		{
			SampleCount = FMath::Min(SampleCount, Settings.MaxSamples);
		}
		return FMath::Max(1, SampleCount);
	}
}

UWishGIMeshPrepCommandlet::UWishGIMeshPrepCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

void UWishGIMeshPrepCommandlet::PrintUsage() const
{
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("WishGI MeshPrep Commandlet Usage:"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -run=WishGIMeshPrep"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -MeshPath=/Game or /Game/Path/SM_Name.SM_Name"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -OutPath=/Game/WishGI/MeshAssoc"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -SampleDensity=100"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -ProbeCount=32"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -AssocPerVertex=2"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -LOD=0"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -KMeansIters=8"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -Seed=1337"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -MinSamples=0"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -MaxSamples=200000"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -Overwrite"));
	UE_LOG(LogWishGIMeshPrep, Display, TEXT("  -Help"));
}

// 还有个主函数呢
// 命令主流程：参数解析 -> 网格扫描 -> 采样聚类 -> 生成并保存关联资产。
int32 UWishGIMeshPrepCommandlet::Main(const FString& Params)
{
	//读命令行参数
	WishGIMeshPrep::FSettings Settings;
	FParse::Value(*Params, TEXT("MeshPath="), Settings.MeshPath);
	FParse::Value(*Params, TEXT("OutPath="), Settings.OutPath);
	FParse::Value(*Params, TEXT("SampleDensity="), Settings.SampleDensity);
	FParse::Value(*Params, TEXT("ProbeCount="), Settings.ProbeCount);
	FParse::Value(*Params, TEXT("AssocPerVertex="), Settings.AssociationsPerVertex);
	FParse::Value(*Params, TEXT("LOD="), Settings.LODIndex);
	FParse::Value(*Params, TEXT("KMeansIters="), Settings.KMeansIterations);
	FParse::Value(*Params, TEXT("Seed="), Settings.Seed);
	FParse::Value(*Params, TEXT("MinSamples="), Settings.MinSamples);
	FParse::Value(*Params, TEXT("MaxSamples="), Settings.MaxSamples);
	Settings.bOverwrite = FParse::Param(*Params, TEXT("Overwrite"));
	Settings.bHelp = FParse::Param(*Params, TEXT("Help")) || FParse::Param(*Params, TEXT("?"));

	//如果用户要帮助，打印用法然后结束
	if (Settings.bHelp)
	{
		PrintUsage();
		return 0;
	}

	//校验参数合法性 例如 OutPath 必须是 /Game/... 这种包路径，数值参数做 clamp
	if (!FPackageName::IsValidLongPackageName(Settings.OutPath))
	{
		UE_LOG(LogWishGIMeshPrep, Error, TEXT("Invalid OutPath '%s'. Use a valid long package path (e.g. /Game/WishGI/MeshAssoc)."), *Settings.OutPath);
		return 1;
	}

	Settings.SampleDensity = FMath::Max(1.0f, Settings.SampleDensity);
	Settings.ProbeCount = FMath::Clamp(Settings.ProbeCount, 1, 256);
	Settings.AssociationsPerVertex = FMath::Clamp(Settings.AssociationsPerVertex, 1, 2);
	Settings.LODIndex = FMath::Max(0, Settings.LODIndex);
	Settings.KMeansIterations = FMath::Clamp(Settings.KMeansIterations, 0, 64);
	Settings.MaxSamples = FMath::Max(Settings.MaxSamples, 1);

	//  4. 扫描目标静态网格 从 MeshPath 找到一个或一批 UStaticMesh。
	TArray<UStaticMesh*> Meshes;
	WishGIMeshPrep::GatherMeshes(Settings.MeshPath, Meshes);
	if (Meshes.Num() == 0)
	{
		UE_LOG(LogWishGIMeshPrep, Error, TEXT("No static meshes found at '%s'."), *Settings.MeshPath);
		return 2;
	}

	UE_LOG(LogWishGIMeshPrep, Display, TEXT("Processing %d mesh(es)."), Meshes.Num());

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	
	int32 SavedCount = 0;
	int32 FailedCount = 0;
	for (UStaticMesh* Mesh : Meshes)
	{
		if (!Mesh)
		{
			continue;
		}

		if (Mesh->GetNumLODs() <= 0)
		{
			UE_LOG(LogWishGIMeshPrep, Warning, TEXT("Skipping '%s': no LOD available."), *Mesh->GetPathName());
			++FailedCount;
			continue;
		}

		const int32 MeshLOD = FMath::Clamp(Settings.LODIndex, 0, Mesh->GetNumLODs() - 1);

		int32 VertexCount = 0;
		float TotalArea = 0.0f;
		TArray<WishGIMeshPrep::FTriangleData> Triangles;
		//遍历每个网格并读取几何 拿到指定 LOD 的顶点和三角形，计算总表面积。
		if (!WishGIMeshPrep::BuildTriangles(Mesh, MeshLOD, VertexCount, Triangles, TotalArea))
		{
			UE_LOG(LogWishGIMeshPrep, Warning, TEXT("Skipping '%s': unable to read valid render triangles for LOD %d."), *Mesh->GetPathName(), MeshLOD);
			++FailedCount;
			continue;
		}
		
		const int32 SampleCount = WishGIMeshPrep::ComputeSampleCount(TotalArea, Settings);
		const int32 EffectiveProbeCount = FMath::Min(Settings.ProbeCount, SampleCount);
		const int32 MeshSeed = HashCombine(GetTypeHash(Mesh->GetPathName()), static_cast<uint32>(Settings.Seed));

		TArray<WishGIMeshPrep::FSamplePoint> Samples;
		//在网格表面采样 按面积加权采样点（密度来自 SampleDensity）
		WishGIMeshPrep::GenerateSurfaceSamples(Triangles, TotalArea, SampleCount, MeshSeed, Samples);
		if (Samples.Num() == 0)
		{
			UE_LOG(LogWishGIMeshPrep, Warning, TEXT("Skipping '%s': sampling failed."), *Mesh->GetPathName());
			++FailedCount;
			continue;
		}

		TArray<FVector> Centers;
		//初始化并优化 probe 中心 对采样点做 KMeans/KMedoids-like 聚类，得到 probe 中心集合。
		WishGIMeshPrep::ComputeProbeCenters(Samples, EffectiveProbeCount, Settings.KMeansIterations, Centers);
		if (Centers.Num() == 0)
		{
			UE_LOG(LogWishGIMeshPrep, Warning, TEXT("Skipping '%s': probe center generation failed."), *Mesh->GetPathName());
			++FailedCount;
			continue;
		}
		
		//计算关联并回传到顶点 每个采样点找 top-2 probe（反距离权重），再通过重心坐标累计到顶点。
		WishGIMeshPrep::ComputeSampleAssociations(Samples, Centers, Settings.AssociationsPerVertex);

		TArray<FWishGIProbeVertexAssociation> VertexAssociations;
		WishGIMeshPrep::BuildVertexAssociations(VertexCount, Triangles, Samples, Centers.Num(), Settings.AssociationsPerVertex, VertexAssociations);

		const FString PackageName = WishGIMeshPrep::MakeAssetPackageName(Settings.OutPath, Mesh);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

		bool bIsNewAsset = false;
		UWishGIMeshAssocAsset* AssocAsset = LoadObject<UWishGIMeshAssocAsset>(nullptr, *ObjectPath);
		if (!AssocAsset)
		{
			//量化并保存资产 每顶点保留 top-2 (probeIndex, weight)，写入 UWishGIMeshAssocAsset 并保存包
			UPackage* Package = CreatePackage(*PackageName);
			AssocAsset = NewObject<UWishGIMeshAssocAsset>(Package, *AssetName, RF_Public | RF_Standalone);
			bIsNewAsset = true;
		}
		else if (!Settings.bOverwrite)
		{
			UE_LOG(LogWishGIMeshPrep, Display, TEXT("Skipping existing asset '%s' (use -Overwrite to replace)."), *ObjectPath);
			continue;
		}

		AssocAsset->SourceMesh = Mesh;
		AssocAsset->LODIndex = MeshLOD;
		AssocAsset->VertexCount = VertexCount;
		AssocAsset->ProbeCount = Centers.Num();
		AssocAsset->SampleDensity = Settings.SampleDensity;
		AssocAsset->AssociationsPerVertex = Settings.AssociationsPerVertex;
		AssocAsset->GeneratedSampleCount = Samples.Num();
		AssocAsset->KMeansIterations = Settings.KMeansIterations;
		AssocAsset->RandomSeed = MeshSeed;
		AssocAsset->VertexAssociations = MoveTemp(VertexAssociations);

		//输出统计并返回结果码 打印 Saved/Failed 数量，决定返回 0 或错误码
		if (bIsNewAsset)
		{
			AssetRegistryModule.AssetCreated(AssocAsset);
		}

		if (WishGIMeshPrep::SaveAssetPackage(AssocAsset->GetOutermost(), AssocAsset))
		{
			++SavedCount;
			UE_LOG(LogWishGIMeshPrep, Display, TEXT("Saved '%s' (LOD=%d, Triangles=%d, Vertices=%d, Samples=%d, Probes=%d)."), *ObjectPath, MeshLOD, Triangles.Num(), VertexCount, Samples.Num(), Centers.Num());
		}
		else
		{
			++FailedCount;
			UE_LOG(LogWishGIMeshPrep, Error, TEXT("Failed to save '%s'."), *ObjectPath);
		}
	}

	UE_LOG(LogWishGIMeshPrep, Display, TEXT("MeshPrep complete. Saved=%d, Failed=%d."), SavedCount, FailedCount);
	return SavedCount > 0 ? 0 : 3;
}

