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
	// ����˵����
	// 1) ���ļ����� Mesh Ԥ�������Ӿ�̬��������ÿ����� probe ������Ȩ�ء�
	// 2) ���Ĳ��裺��ȡ������ -> �����Ȩ���� -> probe ���ľ��� -> Ȩ�ػش����㡣
	// 3) ������ UWishGIMeshAssocAsset���� Bake �׶�������ʱ�ؽ�ʹ�á�
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
		FVector Normal = FVector::UpVector;
		FVector Bary = FVector::ZeroVector;
		FIntVector TriangleVertices = FIntVector::ZeroValue;
		int32 Probe0 = 0;
		int32 Probe1 = 0;
		float ProbeWeight0 = 1.0f;
		float ProbeWeight1 = 0.0f;
	};

	static bool IsObjectPath(const FString& InPath)
	{
		return InPath.Contains(TEXT("."));
	}
	
	/*5. ��Դ���л� (SaveAssetPackage)
	���񣺰���õ���Щ̽��������Ȩ�ء�����������ݣ�������һ���Զ���� UE �ʲ���UWishGIMeshAssocAsset��

	��ֵ����������Ⱦ����ʱ������ֱ�Ӷ�ȡ����ʲ����ɣ�����Ҫ�����κθ��ӵļ��μ��㡣*/
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
	
	/*1. ��������ȡ (BuildTriangles)
	���񣺴� UE4 �� StaticMeshRenderData ��ץȡ���������ݡ�

	�ؼ��㣺��������ÿ�������ε������ͨ�����������������һ�����ۻ������������Ϊ��һ���ľ��Ȳ�����׼����*/
	// ��ȡ LOD �������������Ϣ��Ϊ���������Ȩ������׼����
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
	1. �Ȱ��������ۼ�����������ʷֲ�����
	2. ÿ�����һ�����ֵ�������ҵ���Ӧ�����Ρ�
	3. �ڸ��������ڲ��������������һ�㡣
	4. ���� Sample.Position �� Sample.Bary��

	���壺�����ܶȸ�������ɱ���������С���Ǳ�������/�����Ǳ�Ƿ������

	2. ������� (GenerateSurfaceSamples)
	������ģ�ͱ�����������������㡣

	��ѧʵ�֣�ʹ����������������������ȸ��������������Ȩ���ѡһ�������Σ�Ȼ�������������������һ���㡣

	Ŀ�ģ�ȷ����������ģ�ͱ������������ȵģ������ĵط���࣬С�ĵط����٣�������� WishGI �����жԱ��渲�ǵ�Ҫ��
	*/
	// ������������������ģ�ͱ���㡣
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
			Sample.TriangleVertices = FIntVector(Tri.V0, Tri.V1, Tri.V2);
			Sample.Normal = FVector::CrossProduct(Tri.P1 - Tri.P0, Tri.P2 - Tri.P0).GetSafeNormal();
			if (Sample.Normal.IsNearlyZero())
			{
				Sample.Normal = FVector::UpVector;
			}
			OutSamples.Add(Sample);
		}
	}
	
	/*
	1. �Ӳ��������������ʼ���ġ�
	2. ������
	3. ��ÿ��������ֵ�������ġ�
	4. ÿ�������ġ�
	5. �ڴ������������������ʵ��������Ϊ�����ģ����ӽ� k-medoids ζ������
	
	���壺�õ�һ���ȶ� probe ���ģ�������������
  
	3. ̽����� (ComputeProbeCenters)
	���񣺽���ǧ����������㣬��Ũ�����ɼ�ʮ��������Ĭ�� 32 ����̽�루Probes����

	�㷨��ʵ���� K-Means ���ࣨ��������ʵ�� K-Medoids ��һ�ֱ��壩��

	�߼��������ϵ�����Ѱ���ܴ�����һƬ������ġ�����λ�á������գ���Щ���ĵ���Ǹ�����ľֲ� GI ̽�롣
	*/
	// ͨ�� KMeans/KMedoids-like ������ʼ�� probe ���ġ�
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

	/*4. ����������-̽�롱���� (BuildVertexAssociations)
	���� WishGI ����경�裺

	���񣺼���ģ���ϵ�ÿһ�����㣨Vertex��Ӧ����������̽���Ӱ�죬�����Ƕ��١�

	Ȩ���㷨�����÷������Ȩ��Inverse Distance Weighting������̽��Խ���Ķ��㣬Ȩ��Խ�ߡ�

	����ѹ��������Ȩ�ع�һ��������Ϊ uint8��0-255��������ζ������Ⱦʱ��ÿ������ֻ��Ҫ�洢���ٵ����ݡ�*/
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
	����ʲô��

	1. ��ÿ�������㣬������ probe ��Ȩ�ذ����������̯�������� 3 �����㡣
	2. ÿ��������ۼ�һ�� (probe -> weight)��
	3. ȡȨ������ǰ 2 �� probe��
	4. ��һ���������� uint8��Weight0/Weight1����

	���壺�ѡ������㼶��Ϣ��ѹ���ɡ�ÿ���� 2 �� probe����������ʱ�ɱ��Ѻá�
	*/
	// ��������� probe Ȩ��ͨ�����������ۻ������㣬������ top-2��
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

	static void BuildSurfaceSamples(const TArray<FSamplePoint>& Samples, int32 ProbeCount, TArray<FWishGISurfaceSample>& OutSurfaceSamples)
	{
		OutSurfaceSamples.Reset();
		OutSurfaceSamples.Reserve(Samples.Num());

		for (const FSamplePoint& Sample : Samples)
		{
			FWishGISurfaceSample Entry;
			Entry.LocalPosition = Sample.Position;
			Entry.LocalNormal = Sample.Normal;
			Entry.VertexIndex0 = Sample.TriangleVertices.X;
			Entry.VertexIndex1 = Sample.TriangleVertices.Y;
			Entry.VertexIndex2 = Sample.TriangleVertices.Z;
			Entry.Barycentric = Sample.Bary;
			Entry.ProbeIndex0 = static_cast<uint8>(FMath::Clamp(Sample.Probe0, 0, 255));
			Entry.ProbeIndex1 = static_cast<uint8>(FMath::Clamp(Sample.Probe1, 0, 255));

			const float ClampedW0 = FMath::Clamp(Sample.ProbeWeight0, 0.0f, 1.0f);
			const int32 QuantizedW0 = FMath::Clamp(FMath::RoundToInt(ClampedW0 * 255.0f), 0, 255);
			Entry.Weight0 = static_cast<uint8>(QuantizedW0);
			Entry.Weight1 = static_cast<uint8>(255 - QuantizedW0);

			if (ProbeCount <= 1 || Entry.ProbeIndex0 == Entry.ProbeIndex1)
			{
				Entry.ProbeIndex1 = Entry.ProbeIndex0;
				Entry.Weight0 = 255;
				Entry.Weight1 = 0;
			}

			OutSurfaceSamples.Add(Entry);
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

// ���и���������
// ���������̣��������� -> ����ɨ�� -> �������� -> ���ɲ���������ʲ���
int32 UWishGIMeshPrepCommandlet::Main(const FString& Params)
{
	//�������в���
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

	//����û�Ҫ��������ӡ�÷�Ȼ�����
	if (Settings.bHelp)
	{
		PrintUsage();
		return 0;
	}

	//У������Ϸ��� ���� OutPath ������ /Game/... ���ְ�·������ֵ������ clamp
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

	//  4. ɨ��Ŀ�꾲̬���� �� MeshPath �ҵ�һ����һ�� UStaticMesh��
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
		//����ÿ�����񲢶�ȡ���� �õ�ָ�� LOD �Ķ���������Σ������ܱ������
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
		//������������ �������Ȩ�����㣨�ܶ����� SampleDensity��
		WishGIMeshPrep::GenerateSurfaceSamples(Triangles, TotalArea, SampleCount, MeshSeed, Samples);
		if (Samples.Num() == 0)
		{
			UE_LOG(LogWishGIMeshPrep, Warning, TEXT("Skipping '%s': sampling failed."), *Mesh->GetPathName());
			++FailedCount;
			continue;
		}

		TArray<FVector> Centers;
		//��ʼ�����Ż� probe ���� �Բ������� KMeans/KMedoids-like ���࣬�õ� probe ���ļ��ϡ�
		WishGIMeshPrep::ComputeProbeCenters(Samples, EffectiveProbeCount, Settings.KMeansIterations, Centers);
		if (Centers.Num() == 0)
		{
			UE_LOG(LogWishGIMeshPrep, Warning, TEXT("Skipping '%s': probe center generation failed."), *Mesh->GetPathName());
			++FailedCount;
			continue;
		}
		
		//����������ش������� ÿ���������� top-2 probe��������Ȩ�أ�����ͨ�����������ۼƵ����㡣
		WishGIMeshPrep::ComputeSampleAssociations(Samples, Centers, Settings.AssociationsPerVertex);

		TArray<FWishGIProbeVertexAssociation> VertexAssociations;
		WishGIMeshPrep::BuildVertexAssociations(VertexCount, Triangles, Samples, Centers.Num(), Settings.AssociationsPerVertex, VertexAssociations);

		TArray<FWishGISurfaceSample> SurfaceSamples;
		WishGIMeshPrep::BuildSurfaceSamples(Samples, Centers.Num(), SurfaceSamples);

		const FString PackageName = WishGIMeshPrep::MakeAssetPackageName(Settings.OutPath, Mesh);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

		bool bIsNewAsset = false;
		UWishGIMeshAssocAsset* AssocAsset = LoadObject<UWishGIMeshAssocAsset>(nullptr, *ObjectPath);
		if (!AssocAsset)
		{
			//�����������ʲ� ÿ���㱣�� top-2 (probeIndex, weight)��д�� UWishGIMeshAssocAsset �������
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
		AssocAsset->SurfaceSamples = MoveTemp(SurfaceSamples);
		AssocAsset->VertexAssociations = MoveTemp(VertexAssociations);

		//���ͳ�Ʋ����ؽ���� ��ӡ Saved/Failed �������������� 0 �������
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






