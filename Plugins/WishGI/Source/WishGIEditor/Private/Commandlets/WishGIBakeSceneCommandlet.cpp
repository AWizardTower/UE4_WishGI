#include "Commandlets/WishGIBakeSceneCommandlet.h"

#include "AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

#include "Editor.h"
#include "CollisionQueryParams.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "Math/SHMath.h"
#include "PrecomputedLightVolume.h"
#include "PrecomputedVolumetricLightmap.h"
#include "StaticMeshResources.h"

#include "WishGIMeshAssocAsset.h"
#include "WishGIProbeMapAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogWishGIBakeScene, Log, All);

namespace WishGIBakeScene
{
enum class ETargetSource : uint8
{
	Synthetic,
	PrecomputedVolume,
	Hybrid,
	RayTrace
};

enum class EPrecomputedSource : uint8
{
	Auto,
	VolumetricLightmap,
	SparseSamples
};

// Bake commandlet implementation for offline probe solving and asset packaging.
// Target source supports Synthetic and PrecomputedVolume with switchable precomputed backends.
struct FSettings
{
	FString MapPath;
	FString AssocPath = TEXT("/Game/WishGI/MeshAssoc");
	FString OutPath = TEXT("/Game/WishGI/Bake");
	FString AssetName = TEXT("WishGI_ProbeMap");
	FString TargetSourceName = TEXT("Synthetic");
	ETargetSource TargetSource = ETargetSource::Synthetic;
    FString PrecomputedSourceName = TEXT("Auto");
    EPrecomputedSource PrecomputedSource = EPrecomputedSource::Auto;
	int32 SHOrder = 2;
	int32 Directions = 192;
	float Lambda = 0.1f;
	bool bOverwrite = false;
	bool bHelp = false;
};

struct FDirectionalLightInfo
{
	FVector DirectionToLight = FVector::UpVector;
	FLinearColor Color = FLinearColor::Black;
	bool bCastShadows = true;
};

struct FPointLightInfo
{
	FVector Position = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::Black;
	float AttenuationRadius = 0.0f;
	bool bUseInverseSquaredFalloff = true;
	bool bCastShadows = true;
};

struct FSpotLightInfo
{
	FVector Position = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	FLinearColor Color = FLinearColor::Black;
	float AttenuationRadius = 0.0f;
	float InnerConeCos = 1.0f;
	float OuterConeCos = 1.0f;
	bool bUseInverseSquaredFalloff = true;
	bool bCastShadows = true;
};

struct FSkyLightInfo
{
	FLinearColor UpperColor = FLinearColor::Black;
	FLinearColor LowerColor = FLinearColor::Black;
	float TraceDistance = 200000.0f;
};

struct FTargetContext
{
	ETargetSource Source = ETargetSource::Synthetic;
    EPrecomputedSource PrecomputedSource = EPrecomputedSource::Auto;
	UWorld* World = nullptr;
	TArray<FVector> DirectionSamples;
	TArray<FDirectionalLightInfo> DirectionalLights;
	TArray<FPointLightInfo> PointLights;
	TArray<FSpotLightInfo> SpotLights;
	TArray<FSkyLightInfo> SkyLights;
};

struct FTargetStats
{
	int32 QueryCount = 0;
	int32 ValidCount = 0;
	int32 FallbackVertexCount = 0;
};

struct FVertexTargets
{
	TArray<double> R;
	TArray<double> G;
	TArray<double> B;
	FTargetStats Stats;
	bool bHasAnyRealSample = false;
};

struct FSurfaceSampleTargets
{
	TArray<double> R;
	TArray<double> G;
	TArray<double> B;
	TArray<uint8> ValidMask;
	FTargetStats Stats;
	bool bHasAnyRealSample = false;
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

static FString TargetSourceToString(ETargetSource Source)
{
	switch (Source)
	{
	case ETargetSource::Synthetic:
		return TEXT("Synthetic");
	case ETargetSource::PrecomputedVolume:
		return TEXT("PrecomputedVolume");
	case ETargetSource::Hybrid:
		return TEXT("Hybrid");
	case ETargetSource::RayTrace:
		return TEXT("RayTrace");
	default:
		return TEXT("Unknown");
	}
}

static bool ParseTargetSource(const FString& InName, ETargetSource& OutSource)
{
	if (InName.Equals(TEXT("Synthetic"), ESearchCase::IgnoreCase))
	{
		OutSource = ETargetSource::Synthetic;
		return true;
	}
	if (InName.Equals(TEXT("PrecomputedVolume"), ESearchCase::IgnoreCase) || InName.Equals(TEXT("PrecomputedLightVolume"), ESearchCase::IgnoreCase))
	{
		OutSource = ETargetSource::PrecomputedVolume;
		return true;
	}
	if (InName.Equals(TEXT("Hybrid"), ESearchCase::IgnoreCase))
	{
		OutSource = ETargetSource::Hybrid;
		return true;
	}
	if (InName.Equals(TEXT("RayTrace"), ESearchCase::IgnoreCase))
	{
		OutSource = ETargetSource::RayTrace;
		return true;
	}
	return false;
}

static FString PrecomputedSourceToString(EPrecomputedSource Source)
{
	switch (Source)
	{
	case EPrecomputedSource::Auto:
		return TEXT("Auto");
	case EPrecomputedSource::VolumetricLightmap:
		return TEXT("VLM");
	case EPrecomputedSource::SparseSamples:
		return TEXT("Sparse");
	default:
		return TEXT("Unknown");
	}
}

static bool ParsePrecomputedSource(const FString& InName, EPrecomputedSource& OutSource)
{
	if (InName.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
	{
		OutSource = EPrecomputedSource::Auto;
		return true;
	}
	if (InName.Equals(TEXT("VLM"), ESearchCase::IgnoreCase) || InName.Equals(TEXT("VolumetricLightmap"), ESearchCase::IgnoreCase))
	{
		OutSource = EPrecomputedSource::VolumetricLightmap;
		return true;
	}
	if (InName.Equals(TEXT("Sparse"), ESearchCase::IgnoreCase) || InName.Equals(TEXT("SparseSamples"), ESearchCase::IgnoreCase) || InName.Equals(TEXT("PrecomputedLightVolume"), ESearchCase::IgnoreCase))
	{
		OutSource = EPrecomputedSource::SparseSamples;
		return true;
	}
	return false;
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
static UWorld* LoadWorldForSampling(const FString& MapPath)
{
	if (MapPath.IsEmpty() || !GEditor)
	{
		return nullptr;
	}

	FString MapPackageName = MapPath;
	if (IsObjectPath(MapPackageName))
	{
		MapPackageName = FPackageName::ObjectPathToPackageName(MapPackageName);
	}

	if (!FPackageName::IsValidLongPackageName(MapPackageName))
	{
		return nullptr;
	}

	const FString MapFilename = FPackageName::LongPackageNameToFilename(MapPackageName, FPackageName::GetMapPackageExtension());
	const FString MapLoadCommand = FString::Printf(TEXT("MAP LOAD FILE=\"%s\" TEMPLATE=0 SHOWPROGRESS=0 FEATURELEVEL=3"), *MapFilename);
	GEditor->Exec(nullptr, *MapLoadCommand, *GError);
	FlushAsyncLoading();

	if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
	{
		return EditorWorld;
	}

	UPackage* MapPackage = FindPackage(nullptr, *MapPackageName);
	return MapPackage ? UWorld::FindWorldInPackage(MapPackage) : nullptr;
}

static void BuildDirectionSamples(int32 DirectionCount, TArray<FVector>& OutDirections)
{
	OutDirections.Reset();
	const int32 Count = FMath::Max(1, DirectionCount);
	OutDirections.Reserve(Count);

	const double GoldenAngle = PI * (3.0 - FMath::Sqrt(5.0));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const double T = (static_cast<double>(Index) + 0.5) / static_cast<double>(Count);
		const double Z = 1.0 - 2.0 * T;
		const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
		const double Phi = static_cast<double>(Index) * GoldenAngle;
		const double X = FMath::Cos(Phi) * R;
		const double Y = FMath::Sin(Phi) * R;
		OutDirections.Add(FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z)).GetSafeNormal());
	}
}

static bool ExtractMeshVertexData(const UWishGIMeshAssocAsset* AssocAsset, TArray<FVector>& OutPositions, TArray<FVector>& OutNormals)
{
	OutPositions.Reset();
	OutNormals.Reset();

	const UStaticMesh* SourceMesh = AssocAsset ? AssocAsset->SourceMesh.LoadSynchronous() : nullptr;
	if (!SourceMesh)
	{
		return false;
	}

	const FStaticMeshRenderData* RenderData = SourceMesh->GetRenderData();
	if (!RenderData || !RenderData->LODResources.IsValidIndex(AssocAsset->LODIndex))
	{
		return false;
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[AssocAsset->LODIndex];
	const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;

	const int32 VertexCount = PositionBuffer.GetNumVertices();
	if (VertexCount <= 0)
	{
		return false;
	}

	OutPositions.SetNum(VertexCount);
	OutNormals.SetNum(VertexCount);
	for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		OutPositions[VertexIndex] = PositionBuffer.VertexPosition(VertexIndex);
		FVector Normal = VertexBuffer.VertexTangentZ(VertexIndex);
		if (!Normal.Normalize())
		{
			Normal = FVector::UpVector;
		}
		OutNormals[VertexIndex] = Normal;
	}

	return true;
}

static void GatherMeshInstanceTransforms(UWorld* World, const UStaticMesh* SourceMesh, TArray<FTransform>& OutTransforms)
{
	OutTransforms.Reset();
	if (!World || !SourceMesh)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsPendingKill())
		{
			continue;
		}

		TInlineComponentArray<UStaticMeshComponent*> MeshComponents(Actor);
		Actor->GetComponents(MeshComponents);
		for (UStaticMeshComponent* MeshComponent : MeshComponents)
		{
			if (!MeshComponent || MeshComponent->IsPendingKill())
			{
				continue;
			}

			if (MeshComponent->GetStaticMesh() == SourceMesh)
			{
				OutTransforms.Add(MeshComponent->GetComponentTransform());
			}
		}
	}
}

static float MaxColorComponent(const FLinearColor& Color)
{
	return FMath::Max(Color.R, FMath::Max(Color.G, Color.B));
}

static FLinearColor CompressLightingRange(const FLinearColor& Lighting)
{
	const float R = FMath::Max(0.0f, Lighting.R);
	const float G = FMath::Max(0.0f, Lighting.G);
	const float B = FMath::Max(0.0f, Lighting.B);
	return FLinearColor(
		R / (1.0f + R),
		G / (1.0f + G),
		B / (1.0f + B),
		1.0f);
}

static bool TraceVisibilitySegment(UWorld* World, const FVector& Start, const FVector& End)
{
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WishGIBakeSceneRTVisibility), true);
	QueryParams.bTraceComplex = false;

	FHitResult Hit;
	return !World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams);
}

static bool TraceVisibilityDirection(UWorld* World, const FVector& Start, const FVector& Direction, float Distance)
{
	const FVector SafeDirection = Direction.GetSafeNormal();
	if (!World || SafeDirection.IsNearlyZero() || Distance <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	return TraceVisibilitySegment(World, Start, Start + SafeDirection * Distance);
}

static float ComputeLocalLightFalloff(float Distance, float Radius, bool bUseInverseSquaredFalloff)
{
	if (Radius <= KINDA_SMALL_NUMBER || Distance >= Radius)
	{
		return 0.0f;
	}

	const float NormalizedDistance = FMath::Clamp(Distance / Radius, 0.0f, 1.0f);
	const float Window = FMath::Square(1.0f - NormalizedDistance * NormalizedDistance);
	if (Window <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	if (bUseInverseSquaredFalloff)
	{
		const float DistanceMeters = FMath::Max(Distance * 0.01f, 0.1f);
		return Window / FMath::Square(DistanceMeters);
	}

	return Window * FMath::Square(1.0f - NormalizedDistance);
}

static bool IsLightUsable(const ULightComponent* LightComponent)
{
	return LightComponent
		&& !LightComponent->IsPendingKill()
		&& LightComponent->bAffectsWorld
		&& LightComponent->IsVisible()
		&& MaxColorComponent(LightComponent->GetColoredLightBrightness()) > KINDA_SMALL_NUMBER;
}

static bool IsSkyLightUsable(const USkyLightComponent* SkyLight)
{
	return SkyLight
		&& !SkyLight->IsPendingKill()
		&& SkyLight->bAffectsWorld
		&& SkyLight->IsVisible()
		&& MaxColorComponent(SkyLight->GetLightColor() * SkyLight->Intensity) > KINDA_SMALL_NUMBER;
}

static void GatherRayTraceLights(UWorld* World, FTargetContext& InOutTargetContext)
{
	InOutTargetContext.DirectionalLights.Reset();
	InOutTargetContext.PointLights.Reset();
	InOutTargetContext.SpotLights.Reset();
	InOutTargetContext.SkyLights.Reset();

	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsPendingKill())
		{
			continue;
		}

		TInlineComponentArray<ULightComponent*> LightComponents(Actor);
		Actor->GetComponents(LightComponents);
		for (ULightComponent* LightComponent : LightComponents)
		{
			if (!IsLightUsable(LightComponent))
			{
				continue;
			}

			if (UDirectionalLightComponent* DirectionalLight = Cast<UDirectionalLightComponent>(LightComponent))
			{
				FDirectionalLightInfo Info;
				Info.DirectionToLight = (-DirectionalLight->GetDirection()).GetSafeNormal();
				if (Info.DirectionToLight.IsNearlyZero())
				{
					Info.DirectionToLight = FVector::UpVector;
				}
				Info.Color = DirectionalLight->GetColoredLightBrightness();
				Info.bCastShadows = DirectionalLight->CastShadows != 0;
				InOutTargetContext.DirectionalLights.Add(Info);
				continue;
			}

			if (USpotLightComponent* SpotLight = Cast<USpotLightComponent>(LightComponent))
			{
				FSpotLightInfo Info;
				Info.Position = SpotLight->GetComponentLocation();
				Info.Direction = SpotLight->GetDirection().GetSafeNormal();
				if (Info.Direction.IsNearlyZero())
				{
					Info.Direction = FVector::ForwardVector;
				}
				Info.Color = SpotLight->GetColoredLightBrightness();
				Info.AttenuationRadius = SpotLight->AttenuationRadius;
				const float ClampedInnerCone = FMath::Clamp(SpotLight->InnerConeAngle, 0.0f, 89.0f);
				const float ClampedOuterCone = FMath::Clamp(SpotLight->OuterConeAngle, ClampedInnerCone + 0.001f, 89.0f);
				Info.InnerConeCos = FMath::Cos(FMath::DegreesToRadians(ClampedInnerCone));
				Info.OuterConeCos = FMath::Cos(FMath::DegreesToRadians(ClampedOuterCone));
				Info.bUseInverseSquaredFalloff = SpotLight->bUseInverseSquaredFalloff != 0;
				Info.bCastShadows = SpotLight->CastShadows != 0;
				InOutTargetContext.SpotLights.Add(Info);
				continue;
			}

			if (UPointLightComponent* PointLight = Cast<UPointLightComponent>(LightComponent))
			{
				FPointLightInfo Info;
				Info.Position = PointLight->GetComponentLocation();
				Info.Color = PointLight->GetColoredLightBrightness();
				Info.AttenuationRadius = PointLight->AttenuationRadius;
				Info.bUseInverseSquaredFalloff = PointLight->bUseInverseSquaredFalloff != 0;
				Info.bCastShadows = PointLight->CastShadows != 0;
				InOutTargetContext.PointLights.Add(Info);
			}
		}

		TInlineComponentArray<USkyLightComponent*> SkyLightComponents(Actor);
		Actor->GetComponents(SkyLightComponents);
		for (USkyLightComponent* SkyLight : SkyLightComponents)
		{
			if (!IsSkyLightUsable(SkyLight))
			{
				continue;
			}

			FSkyLightInfo Info;
			const FLinearColor SkyColor = SkyLight->GetLightColor() * SkyLight->Intensity;
			Info.UpperColor = SkyColor;
			Info.LowerColor = SkyLight->bLowerHemisphereIsBlack ? (SkyLight->LowerHemisphereColor * SkyLight->Intensity) : SkyColor;
			Info.TraceDistance = FMath::Max(10000.0f, SkyLight->SkyDistanceThreshold > 0.0f ? SkyLight->SkyDistanceThreshold * 4.0f : 200000.0f);
			InOutTargetContext.SkyLights.Add(Info);
		}
	}

	UE_LOG(LogWishGIBakeScene, Display, TEXT("Gathered RT lights: Directional=%d, Point=%d, Spot=%d, Sky=%d"),
		InOutTargetContext.DirectionalLights.Num(),
		InOutTargetContext.PointLights.Num(),
		InOutTargetContext.SpotLights.Num(),
		InOutTargetContext.SkyLights.Num());
}

static FLinearColor SampleRayTracedLightingAt(const FTargetContext& TargetContext, const FVector& WorldPosition, const FVector& WorldNormal)
{
	if (!TargetContext.World)
	{
		return FLinearColor::Black;
	}

	FVector SafeNormal = WorldNormal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero())
	{
		SafeNormal = FVector::UpVector;
	}

	const FVector TraceStart = WorldPosition + SafeNormal * 2.0f;
	FLinearColor Accumulated = FLinearColor::Black;

	for (const FDirectionalLightInfo& Light : TargetContext.DirectionalLights)
	{
		const float NdotL = FMath::Max(0.0f, FVector::DotProduct(SafeNormal, Light.DirectionToLight));
		if (NdotL <= 0.0f)
		{
			continue;
		}

		if (Light.bCastShadows && !TraceVisibilityDirection(TargetContext.World, TraceStart, Light.DirectionToLight, 200000.0f))
		{
			continue;
		}

		Accumulated += Light.Color * NdotL;
	}

	for (const FPointLightInfo& Light : TargetContext.PointLights)
	{
		const FVector ToLight = Light.Position - TraceStart;
		const float Distance = ToLight.Size();
		if (Distance <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float Falloff = ComputeLocalLightFalloff(Distance, Light.AttenuationRadius, Light.bUseInverseSquaredFalloff);
		if (Falloff <= 0.0f)
		{
			continue;
		}

		const FVector DirectionToLight = ToLight / Distance;
		const float NdotL = FMath::Max(0.0f, FVector::DotProduct(SafeNormal, DirectionToLight));
		if (NdotL <= 0.0f)
		{
			continue;
		}

		if (Light.bCastShadows && !TraceVisibilitySegment(TargetContext.World, TraceStart, Light.Position - DirectionToLight))
		{
			continue;
		}

		Accumulated += Light.Color * (NdotL * Falloff);
	}

	for (const FSpotLightInfo& Light : TargetContext.SpotLights)
	{
		const FVector ToLight = Light.Position - TraceStart;
		const float Distance = ToLight.Size();
		if (Distance <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float Falloff = ComputeLocalLightFalloff(Distance, Light.AttenuationRadius, Light.bUseInverseSquaredFalloff);
		if (Falloff <= 0.0f)
		{
			continue;
		}

		const FVector DirectionToLight = ToLight / Distance;
		const float NdotL = FMath::Max(0.0f, FVector::DotProduct(SafeNormal, DirectionToLight));
		if (NdotL <= 0.0f)
		{
			continue;
		}

		const float CosTheta = FVector::DotProduct(Light.Direction, -DirectionToLight);
		if (CosTheta <= Light.OuterConeCos)
		{
			continue;
		}

		float ConeFactor = 1.0f;
		if (Light.InnerConeCos > Light.OuterConeCos)
		{
			ConeFactor = FMath::Clamp((CosTheta - Light.OuterConeCos) / (Light.InnerConeCos - Light.OuterConeCos), 0.0f, 1.0f);
		}

		if (Light.bCastShadows && !TraceVisibilitySegment(TargetContext.World, TraceStart, Light.Position - DirectionToLight))
		{
			continue;
		}

		Accumulated += Light.Color * (NdotL * Falloff * ConeFactor);
	}

	for (const FSkyLightInfo& Sky : TargetContext.SkyLights)
	{
		double SumWeight = 0.0;
		FLinearColor SkyAccumulated = FLinearColor::Black;
		for (const FVector& Direction : TargetContext.DirectionSamples)
		{
			const float Weight = FMath::Max(0.0f, FVector::DotProduct(SafeNormal, Direction));
			if (Weight <= 0.0f)
			{
				continue;
			}

			if (!TraceVisibilityDirection(TargetContext.World, TraceStart, Direction, Sky.TraceDistance))
			{
				continue;
			}

			SkyAccumulated += (Direction.Z >= 0.0f ? Sky.UpperColor : Sky.LowerColor) * Weight;
			SumWeight += Weight;
		}

		if (SumWeight > 1e-9)
		{
			Accumulated += SkyAccumulated / static_cast<float>(SumWeight);
		}
	}

	return CompressLightingRange(Accumulated);
}

static const FPrecomputedLightVolumeData* GetSparseBuildDataForLevel(const ULevel* Level)
{
	if (!Level || !Level->MapBuildData || !Level->LevelBuildDataId.IsValid())
	{
		return nullptr;
	}

	return Level->MapBuildData->GetLevelPrecomputedLightVolumeBuildData(Level->LevelBuildDataId);
}

static const FPrecomputedVolumetricLightmapData* GetVolumetricBuildDataForLevel(const ULevel* Level)
{
	if (!Level || !Level->MapBuildData || !Level->LevelBuildDataId.IsValid())
	{
		return nullptr;
	}

	return Level->MapBuildData->GetLevelPrecomputedVolumetricLightmapBuildData(Level->LevelBuildDataId);
}

static const TCHAR* VolumeLightingMethodToString(EVolumeLightingMethod Method)
{
	switch (Method)
	{
	case VLM_VolumetricLightmap:
		return TEXT("VolumetricLightmap");
	case VLM_SparseVolumeLightingSamples:
		return TEXT("SparseVolumeLightingSamples");
	default:
		return TEXT("Unknown");
	}
}

static void LogPrecomputedDataSummary(const UWorld* World)
{
	if (!World)
	{
		return;
	}

	const AWorldSettings* WorldSettings = World->GetWorldSettings();
	UE_LOG(LogWishGIBakeScene, Display, TEXT("Loaded precomputed world '%s': VolumeLightingMethod=%s, Levels=%d"),
		*World->GetPathName(),
		VolumeLightingMethodToString(WorldSettings ? WorldSettings->LightmassSettings.VolumeLightingMethod : VLM_VolumetricLightmap),
		World->GetLevels().Num());

	for (ULevel* Level : World->GetLevels())
	{
		const FPrecomputedLightVolumeData* SparseBuildData = GetSparseBuildDataForLevel(Level);
		const FPrecomputedVolumetricLightmapData* VolumetricBuildData = GetVolumetricBuildDataForLevel(Level);
		const FBox VolumetricBounds = VolumetricBuildData ? VolumetricBuildData->GetBounds() : FBox(EForceInit::ForceInit);
		UE_LOG(LogWishGIBakeScene, Display,
			TEXT("  Level='%s' BuildDataId=%s MapBuildData=%s Sparse=%s SparseInitialized=%d VLM=%s VLMBoundsValid=%d BrickSize=%d IndirectionDims=%s BrickDims=%s"),
			*GetPathNameSafe(Level),
			*Level->LevelBuildDataId.ToString(),
			Level->MapBuildData ? *Level->MapBuildData->GetPathName() : TEXT("None"),
			SparseBuildData ? TEXT("Yes") : TEXT("No"),
			SparseBuildData && SparseBuildData->IsInitialized() ? 1 : 0,
			VolumetricBuildData ? TEXT("Yes") : TEXT("No"),
			VolumetricBounds.IsValid ? 1 : 0,
			VolumetricBuildData ? VolumetricBuildData->BrickSize : 0,
			VolumetricBuildData ? *VolumetricBuildData->IndirectionTextureDimensions.ToString() : TEXT("(0,0,0)"),
			VolumetricBuildData ? *VolumetricBuildData->BrickDataDimensions.ToString() : TEXT("(0,0,0)"));
	}
}

static bool SampleIncidentRadianceFromSparseAt(const UWorld* World, const FVector& WorldPosition, FSHVectorRGB3& OutIncident)
{
	if (!World || World->IsPendingKill())
	{
		return false;
	}

	// FPrecomputedLightVolume's destructor is not exported from Engine in UE4.27,
	// so keep a single commandlet-lifetime instance and never destroy it.
	static FPrecomputedLightVolume* SparseInterpolator = nullptr;
	if (!SparseInterpolator)
	{
		void* Memory = FMemory::Malloc(sizeof(FPrecomputedLightVolume));
		SparseInterpolator = new (Memory) FPrecomputedLightVolume();
	}

	float TotalWeight = 0.0f;
	FSHVectorRGB3 AccumulatedIncident;

	for (ULevel* Level : World->GetLevels())
	{
		const FPrecomputedLightVolumeData* SparseBuildData = GetSparseBuildDataForLevel(Level);
		if (!Level || !World->Scene || !SparseBuildData || !SparseBuildData->IsInitialized())
		{
			continue;
		}

		float Weight = 0.0f;
		float DirectionalShadowing = 0.0f;
		FSHVectorRGB3 Incident;
		FVector SkyBentNormal = FVector::ZeroVector;

		SparseInterpolator->SetData(SparseBuildData, World->Scene);
		SparseInterpolator->InterpolateIncidentRadiancePoint(WorldPosition, Weight, DirectionalShadowing, Incident, SkyBentNormal);

		if (Weight > KINDA_SMALL_NUMBER)
		{
			AccumulatedIncident += Incident;
			TotalWeight += Weight;
		}
	}

	if (TotalWeight <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	OutIncident = AccumulatedIncident / TotalWeight;
	return true;
}

static bool DecodeVolumetricLightmapIncidentRadiance(
	const FPrecomputedVolumetricLightmapData& VolumetricLightmapData,
	const FVector& BrickTextureCoordinate,
	FSHVectorRGB3& OutIncident)
{
	if (VolumetricLightmapData.BrickData.AmbientVector.Data.Num() <= 0)
	{
		return false;
	}

	for (int32 CoefficientIndex = 0; CoefficientIndex < UE_ARRAY_COUNT(VolumetricLightmapData.BrickData.SHCoefficients); ++CoefficientIndex)
	{
		if (VolumetricLightmapData.BrickData.SHCoefficients[CoefficientIndex].Data.Num() <= 0)
		{
			return false;
		}
	}

	const FVector AmbientVector = (FVector)FilteredVolumeLookup<FFloat3Packed>(
		BrickTextureCoordinate,
		VolumetricLightmapData.BrickDataDimensions,
		(const FFloat3Packed*)VolumetricLightmapData.BrickData.AmbientVector.Data.GetData());

	auto ReadSHCoefficient = [&BrickTextureCoordinate, &VolumetricLightmapData, &AmbientVector](uint32 CoefficientIndex)
	{
		const FLinearColor SHDenormalizationScales0(
			0.488603f / 0.282095f,
			0.488603f / 0.282095f,
			0.488603f / 0.282095f,
			1.092548f / 0.282095f);

		const FLinearColor SHDenormalizationScales1(
			1.092548f / 0.282095f,
			4.0f * 0.315392f / 0.282095f,
			1.092548f / 0.282095f,
			2.0f * 0.546274f / 0.282095f);

		FLinearColor SHCoefficientEncoded = FilteredVolumeLookup<FColor>(
			BrickTextureCoordinate,
			VolumetricLightmapData.BrickDataDimensions,
			(const FColor*)VolumetricLightmapData.BrickData.SHCoefficients[CoefficientIndex].Data.GetData());

		Swap(SHCoefficientEncoded.R, SHCoefficientEncoded.B);

		const FLinearColor& DenormalizationScales = ((CoefficientIndex & 1) == 0) ? SHDenormalizationScales0 : SHDenormalizationScales1;
		return FVector4((SHCoefficientEncoded * 2.0f - FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)) * AmbientVector[CoefficientIndex / 2] * DenormalizationScales);
	};

	auto BuildSHVector = [](float Ambient, const FVector4& Coeffs0, const FVector4& Coeffs1)
	{
		FSHVector3 Result;
		Result.V[0] = Ambient;
		FMemory::Memcpy(&Result.V[1], &Coeffs0, sizeof(Coeffs0));
		FMemory::Memcpy(&Result.V[5], &Coeffs1, sizeof(Coeffs1));
		return Result;
	};

	OutIncident.R = BuildSHVector(AmbientVector.X, ReadSHCoefficient(0), ReadSHCoefficient(1));
	OutIncident.G = BuildSHVector(AmbientVector.Y, ReadSHCoefficient(2), ReadSHCoefficient(3));
	OutIncident.B = BuildSHVector(AmbientVector.Z, ReadSHCoefficient(4), ReadSHCoefficient(5));

	if (VolumetricLightmapData.BrickData.LQLightColor.Data.Num() > 0 && VolumetricLightmapData.BrickData.LQLightDirection.Data.Num() > 0)
	{
		const FLinearColor LQLightColor = FilteredVolumeLookup<FFloat3Packed>(
			BrickTextureCoordinate,
			VolumetricLightmapData.BrickDataDimensions,
			(const FFloat3Packed*)VolumetricLightmapData.BrickData.LQLightColor.Data.GetData());

		FVector LQLightDirection = (FVector)FilteredVolumeLookup<FColor>(
			BrickTextureCoordinate,
			VolumetricLightmapData.BrickDataDimensions,
			(const FColor*)VolumetricLightmapData.BrickData.LQLightDirection.Data.GetData());

		Swap(LQLightDirection.X, LQLightDirection.Z);
		LQLightDirection = LQLightDirection * 2.0f - FVector(1.0f, 1.0f, 1.0f);
		if (!LQLightDirection.Normalize())
		{
			LQLightDirection = FVector::UpVector;
		}

		OutIncident.AddIncomingRadiance(LQLightColor, 1.0f, LQLightDirection);
	}

	return true;
}

static bool SampleIncidentRadianceFromVLMAt(const UWorld* World, const FVector& WorldPosition, FSHVectorRGB3& OutIncident)
{
	if (!World || World->IsPendingKill())
	{
		return false;
	}

	int32 ValidLevelCount = 0;
	FSHVectorRGB3 AccumulatedIncident;

	for (ULevel* Level : World->GetLevels())
	{
		const FPrecomputedVolumetricLightmapData* GlobalData = GetVolumetricBuildDataForLevel(Level);
		if (!GlobalData)
		{
			continue;
		}

		if (!GlobalData->GetBounds().IsValid || GlobalData->IndirectionTexture.Data.Num() <= 0 || GlobalData->BrickSize <= 0)
		{
			continue;
		}

		const FVector IndirectionCoordinate = ComputeIndirectionCoordinate(WorldPosition, GlobalData->GetBounds(), GlobalData->IndirectionTextureDimensions);

		FIntVector BrickOffset = FIntVector::ZeroValue;
		int32 BrickSize = 0;
		int32 SubLevelIndex = 0;

		if (GlobalData->CPUSubLevelBrickDataList.Num() > 0 && GlobalData->CPUSubLevelIndirectionTable.Num() > 0)
		{
			SampleIndirectionTextureWithSubLevel(
				IndirectionCoordinate,
				GlobalData->IndirectionTextureDimensions,
				GlobalData->IndirectionTexture.Data.GetData(),
				GlobalData->CPUSubLevelIndirectionTable,
				BrickOffset,
				BrickSize,
				SubLevelIndex);
		}
		else
		{
			SampleIndirectionTexture(
				IndirectionCoordinate,
				GlobalData->IndirectionTextureDimensions,
				GlobalData->IndirectionTexture.Data.GetData(),
				BrickOffset,
				BrickSize);
		}

		if (BrickSize <= 0)
		{
			continue;
		}

		const FPrecomputedVolumetricLightmapData* SampleData = nullptr;
		if (GlobalData->CPUSubLevelBrickDataList.IsValidIndex(SubLevelIndex) && GlobalData->CPUSubLevelBrickDataList[SubLevelIndex])
		{
			SampleData = GlobalData->CPUSubLevelBrickDataList[SubLevelIndex];
		}
		if (!SampleData)
		{
			SampleData = GlobalData;
		}

		if (SampleData->BrickDataDimensions.GetMin() <= 0)
		{
			continue;
		}

		const FVector BrickTextureCoordinate = ComputeBrickTextureCoordinate(
			IndirectionCoordinate,
			BrickOffset,
			BrickSize,
			SampleData->BrickSize);

		FSHVectorRGB3 Incident;
		if (!DecodeVolumetricLightmapIncidentRadiance(*SampleData, BrickTextureCoordinate, Incident))
		{
			continue;
		}

		AccumulatedIncident += Incident;
		ValidLevelCount += 1;
	}

	if (ValidLevelCount <= 0)
	{
		return false;
	}

	OutIncident = AccumulatedIncident / static_cast<float>(ValidLevelCount);
	return true;
}

static bool SampleIncidentRadianceAt(const FTargetContext& TargetContext, const FVector& WorldPosition, FSHVectorRGB3& OutIncident)
{
	if (!TargetContext.World || TargetContext.World->IsPendingKill())
	{
		return false;
	}

	switch (TargetContext.PrecomputedSource)
	{
	case EPrecomputedSource::VolumetricLightmap:
		return SampleIncidentRadianceFromVLMAt(TargetContext.World, WorldPosition, OutIncident);
	case EPrecomputedSource::SparseSamples:
		return SampleIncidentRadianceFromSparseAt(TargetContext.World, WorldPosition, OutIncident);
	case EPrecomputedSource::Auto:
	default:
		if (SampleIncidentRadianceFromVLMAt(TargetContext.World, WorldPosition, OutIncident))
		{
			return true;
		}
		return SampleIncidentRadianceFromSparseAt(TargetContext.World, WorldPosition, OutIncident);
	}
}
static FLinearColor IntegrateEffectiveDirections(const FSHVectorRGB3& Incident, const FVector& WorldNormal, const TArray<FVector>& Directions)
{
	FVector SafeNormal = WorldNormal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero())
	{
		SafeNormal = FVector::UpVector;
	}
	double SumW = 0.0;
	double SumR = 0.0;
	double SumG = 0.0;
	double SumB = 0.0;

	for (const FVector& Direction : Directions)
	{
		const double W = FMath::Max(0.0, static_cast<double>(FVector::DotProduct(SafeNormal, Direction)));
		if (W <= 0.0)
		{
			continue;
		}

		const FSHVector3 Basis = FSHVector3::SHBasisFunction(Direction);
		const FLinearColor Radiance = Dot(Incident, Basis);
		SumW += W;
		SumR += W * FMath::Max(0.0, static_cast<double>(Radiance.R));
		SumG += W * FMath::Max(0.0, static_cast<double>(Radiance.G));
		SumB += W * FMath::Max(0.0, static_cast<double>(Radiance.B));
	}

	if (SumW <= 1e-9)
	{
		return FLinearColor::Black;
	}

	return FLinearColor(
		static_cast<float>(SumR / SumW),
		static_cast<float>(SumG / SumW),
		static_cast<float>(SumB / SumW),
		1.0f);
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

static void BuildVertexTargetsSynthetic(uint32 MeshHash, int32 VertexIndex, double& OutR, double& OutG, double& OutB)
{
	const uint32 SeedR = HashCombine(MeshHash, static_cast<uint32>(VertexIndex * 3 + 11));
	const uint32 SeedG = HashCombine(MeshHash, static_cast<uint32>(VertexIndex * 3 + 37));
	const uint32 SeedB = HashCombine(MeshHash, static_cast<uint32>(VertexIndex * 3 + 79));

	OutR = 0.15 + 0.85 * HashToUnit(SeedR);
	OutG = 0.15 + 0.85 * HashToUnit(SeedG);
	OutB = 0.15 + 0.85 * HashToUnit(SeedB);
}
static void BuildSyntheticTargets(const UWishGIMeshAssocAsset* AssocAsset, FVertexTargets& OutTargets)
{
	const int32 VertexCount = AssocAsset ? AssocAsset->VertexAssociations.Num() : 0;
	OutTargets.R.SetNum(VertexCount);
	OutTargets.G.SetNum(VertexCount);
	OutTargets.B.SetNum(VertexCount);

	const uint32 MeshHash = AssocAsset ? GetTypeHash(AssocAsset->GetPathName()) : 0u;
	for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		BuildVertexTargetsSynthetic(MeshHash, VertexIndex, R, G, B);
		OutTargets.R[VertexIndex] = R;
		OutTargets.G[VertexIndex] = G;
		OutTargets.B[VertexIndex] = B;
	}
}

static bool BuildSurfaceSampleTargetsFromRayTrace(const UWishGIMeshAssocAsset* AssocAsset, const FTargetContext& TargetContext, FSurfaceSampleTargets& OutTargets)
{
	OutTargets = FSurfaceSampleTargets();
	if (!AssocAsset || !TargetContext.World || AssocAsset->SurfaceSamples.Num() <= 0)
	{
		return false;
	}

	const UStaticMesh* SourceMesh = AssocAsset->SourceMesh.LoadSynchronous();
	TArray<FTransform> InstanceTransforms;
	GatherMeshInstanceTransforms(TargetContext.World, SourceMesh, InstanceTransforms);
	if (InstanceTransforms.Num() == 0)
	{
		return false;
	}

	const int32 SampleCount = AssocAsset->SurfaceSamples.Num();
	OutTargets.R.Init(0.0, SampleCount);
	OutTargets.G.Init(0.0, SampleCount);
	OutTargets.B.Init(0.0, SampleCount);
	OutTargets.ValidMask.Init(0, SampleCount);

	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const FWishGISurfaceSample& SurfaceSample = AssocAsset->SurfaceSamples[SampleIndex];
		double AccR = 0.0;
		double AccG = 0.0;
		double AccB = 0.0;
		int32 ValidSamplesForSurfaceSample = 0;

		for (const FTransform& Transform : InstanceTransforms)
		{
			const FVector WorldPosition = Transform.TransformPosition(SurfaceSample.LocalPosition);
			FVector WorldNormal = Transform.TransformVectorNoScale(SurfaceSample.LocalNormal).GetSafeNormal();
			if (WorldNormal.IsNearlyZero())
			{
				WorldNormal = FVector::UpVector;
			}

			OutTargets.Stats.QueryCount += 1;
			const FLinearColor Sampled = SampleRayTracedLightingAt(TargetContext, WorldPosition, WorldNormal);
			OutTargets.Stats.ValidCount += 1;
			AccR += static_cast<double>(Sampled.R);
			AccG += static_cast<double>(Sampled.G);
			AccB += static_cast<double>(Sampled.B);
			ValidSamplesForSurfaceSample += 1;
		}

		if (ValidSamplesForSurfaceSample <= 0)
		{
			continue;
		}

		const double InvValidCount = 1.0 / static_cast<double>(ValidSamplesForSurfaceSample);
		OutTargets.R[SampleIndex] = AccR * InvValidCount;
		OutTargets.G[SampleIndex] = AccG * InvValidCount;
		OutTargets.B[SampleIndex] = AccB * InvValidCount;
		OutTargets.ValidMask[SampleIndex] = 1;
		OutTargets.bHasAnyRealSample = true;
	}

	return OutTargets.bHasAnyRealSample;
}

static bool BuildSurfaceSampleTargetsFromPrecomputedVolume(const UWishGIMeshAssocAsset* AssocAsset, const FTargetContext& TargetContext, FSurfaceSampleTargets& OutTargets)
{
	OutTargets = FSurfaceSampleTargets();
	if (!AssocAsset || !TargetContext.World || AssocAsset->SurfaceSamples.Num() <= 0)
	{
		return false;
	}

	const UStaticMesh* SourceMesh = AssocAsset->SourceMesh.LoadSynchronous();
	TArray<FTransform> InstanceTransforms;
	GatherMeshInstanceTransforms(TargetContext.World, SourceMesh, InstanceTransforms);
	if (InstanceTransforms.Num() == 0)
	{
		return false;
	}

	const int32 SampleCount = AssocAsset->SurfaceSamples.Num();
	OutTargets.R.Init(0.0, SampleCount);
	OutTargets.G.Init(0.0, SampleCount);
	OutTargets.B.Init(0.0, SampleCount);
	OutTargets.ValidMask.Init(0, SampleCount);

	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const FWishGISurfaceSample& SurfaceSample = AssocAsset->SurfaceSamples[SampleIndex];
		double AccR = 0.0;
		double AccG = 0.0;
		double AccB = 0.0;
		int32 ValidSamplesForSurfaceSample = 0;

		for (const FTransform& Transform : InstanceTransforms)
		{
			const FVector WorldPosition = Transform.TransformPosition(SurfaceSample.LocalPosition);
			FVector WorldNormal = Transform.TransformVectorNoScale(SurfaceSample.LocalNormal).GetSafeNormal();
			if (WorldNormal.IsNearlyZero())
			{
				WorldNormal = FVector::UpVector;
			}

			OutTargets.Stats.QueryCount += 1;

			FSHVectorRGB3 Incident;
			if (!SampleIncidentRadianceAt(TargetContext, WorldPosition, Incident))
			{
				continue;
			}

			OutTargets.Stats.ValidCount += 1;
			const FLinearColor Sampled = IntegrateEffectiveDirections(Incident, WorldNormal, TargetContext.DirectionSamples);
			AccR += static_cast<double>(Sampled.R);
			AccG += static_cast<double>(Sampled.G);
			AccB += static_cast<double>(Sampled.B);
			ValidSamplesForSurfaceSample += 1;
		}

		if (ValidSamplesForSurfaceSample <= 0)
		{
			continue;
		}

		const double InvValidCount = 1.0 / static_cast<double>(ValidSamplesForSurfaceSample);
		OutTargets.R[SampleIndex] = AccR * InvValidCount;
		OutTargets.G[SampleIndex] = AccG * InvValidCount;
		OutTargets.B[SampleIndex] = AccB * InvValidCount;
		OutTargets.ValidMask[SampleIndex] = 1;
		OutTargets.bHasAnyRealSample = true;
	}

	return OutTargets.bHasAnyRealSample;
}

static void AddAssociationWeight(const FWishGIProbeVertexAssociation& Assoc, double WeightScale, int32 MeshProbeCount, TMap<int32, double>& InOutWeights)
{
	if (WeightScale <= 0.0 || MeshProbeCount <= 0)
	{
		return;
	}

	const int32 Probe0 = FMath::Clamp<int32>(Assoc.ProbeIndex0, 0, MeshProbeCount - 1);
	const int32 Probe1 = FMath::Clamp<int32>(Assoc.ProbeIndex1, 0, MeshProbeCount - 1);
	const double Weight0 = static_cast<double>(Assoc.Weight0) / 255.0;
	const double Weight1 = static_cast<double>(Assoc.Weight1) / 255.0;

	InOutWeights.FindOrAdd(Probe0) += WeightScale * Weight0;
	if (Probe1 != Probe0 && Weight1 > 1e-6)
	{
		InOutWeights.FindOrAdd(Probe1) += WeightScale * Weight1;
	}
}

static void AccumulateSampleConstraint(
	const TMap<int32, double>& SampleWeights,
	double TargetValue,
	float Lambda,
	FLinearSystemDense& InOutSystem)
{
	TArray<TPair<int32, double>> Terms;
	Terms.Reserve(SampleWeights.Num());
	double WeightSum = 0.0;

	for (const TPair<int32, double>& Pair : SampleWeights)
	{
		if (Pair.Value > 1e-8)
		{
			Terms.Add(Pair);
			WeightSum += Pair.Value;
		}
	}

	if (Terms.Num() == 0 || WeightSum <= 1e-8)
	{
		return;
	}

	for (TPair<int32, double>& Term : Terms)
	{
		Term.Value /= WeightSum;
	}

	for (int32 RowIndex = 0; RowIndex < Terms.Num(); ++RowIndex)
	{
		const int32 ProbeRow = Terms[RowIndex].Key;
		const double WeightRow = Terms[RowIndex].Value;
		InOutSystem.B[ProbeRow] += WeightRow * TargetValue;

		for (int32 ColIndex = 0; ColIndex < Terms.Num(); ++ColIndex)
		{
			const int32 ProbeCol = Terms[ColIndex].Key;
			const double WeightCol = Terms[ColIndex].Value;
			At(InOutSystem, ProbeRow, ProbeCol) += WeightRow * WeightCol;
		}
	}

	for (int32 Index = 0; Index + 1 < Terms.Num(); ++Index)
	{
		const int32 ProbeA = Terms[Index].Key;
		const int32 ProbeB = Terms[Index + 1].Key;
		const double EdgeReg = static_cast<double>(Lambda) * 0.05 * (Terms[Index].Value + Terms[Index + 1].Value);
		At(InOutSystem, ProbeA, ProbeA) += EdgeReg;
		At(InOutSystem, ProbeB, ProbeB) += EdgeReg;
		At(InOutSystem, ProbeA, ProbeB) -= EdgeReg;
		At(InOutSystem, ProbeB, ProbeA) -= EdgeReg;
	}
}
static void AccumulateSurfaceSampleToVertex(int32 VertexIndex, double Weight, const FLinearColor& Sampled, TArray<double>& InOutWeights, FVertexTargets& InOutTargets)
{
	if (!InOutWeights.IsValidIndex(VertexIndex) || Weight <= 0.0)
	{
		return;
	}

	InOutWeights[VertexIndex] += Weight;
	InOutTargets.R[VertexIndex] += static_cast<double>(Sampled.R) * Weight;
	InOutTargets.G[VertexIndex] += static_cast<double>(Sampled.G) * Weight;
	InOutTargets.B[VertexIndex] += static_cast<double>(Sampled.B) * Weight;
}

static bool BuildPrecomputedVolumeTargetsFromSurfaceSamples(const UWishGIMeshAssocAsset* AssocAsset, const FTargetContext& TargetContext, FVertexTargets& OutTargets)
{
	if (!AssocAsset || !TargetContext.World || AssocAsset->SurfaceSamples.Num() <= 0)
	{
		return false;
	}

	const UStaticMesh* SourceMesh = AssocAsset->SourceMesh.LoadSynchronous();
	TArray<FTransform> InstanceTransforms;
	GatherMeshInstanceTransforms(TargetContext.World, SourceMesh, InstanceTransforms);
	if (InstanceTransforms.Num() == 0)
	{
		return false;
	}

	const int32 AssocVertexCount = AssocAsset->VertexAssociations.Num();
	if (AssocVertexCount <= 0)
	{
		return false;
	}

	OutTargets.R.Init(0.0, AssocVertexCount);
	OutTargets.G.Init(0.0, AssocVertexCount);
	OutTargets.B.Init(0.0, AssocVertexCount);

	TArray<double> VertexWeightSums;
	VertexWeightSums.Init(0.0, AssocVertexCount);

	for (const FWishGISurfaceSample& SurfaceSample : AssocAsset->SurfaceSamples)
	{
		double AccR = 0.0;
		double AccG = 0.0;
		double AccB = 0.0;
		int32 ValidSamplesForSurfaceSample = 0;

		for (const FTransform& Transform : InstanceTransforms)
		{
			const FVector WorldPosition = Transform.TransformPosition(SurfaceSample.LocalPosition);
			FVector WorldNormal = Transform.TransformVectorNoScale(SurfaceSample.LocalNormal).GetSafeNormal();
			if (WorldNormal.IsNearlyZero())
			{
				WorldNormal = FVector::UpVector;
			}

			OutTargets.Stats.QueryCount += 1;

			FSHVectorRGB3 Incident;
			if (!SampleIncidentRadianceAt(TargetContext, WorldPosition, Incident))
			{
				continue;
			}

			OutTargets.Stats.ValidCount += 1;
			const FLinearColor Sampled = IntegrateEffectiveDirections(Incident, WorldNormal, TargetContext.DirectionSamples);
			AccR += static_cast<double>(Sampled.R);
			AccG += static_cast<double>(Sampled.G);
			AccB += static_cast<double>(Sampled.B);
			ValidSamplesForSurfaceSample += 1;
		}

		if (ValidSamplesForSurfaceSample <= 0)
		{
			continue;
		}

		OutTargets.bHasAnyRealSample = true;
		const double InvValidCount = 1.0 / static_cast<double>(ValidSamplesForSurfaceSample);
		const FLinearColor AveragedSample(
			static_cast<float>(AccR * InvValidCount),
			static_cast<float>(AccG * InvValidCount),
			static_cast<float>(AccB * InvValidCount),
			1.0f);

		AccumulateSurfaceSampleToVertex(SurfaceSample.VertexIndex0, static_cast<double>(SurfaceSample.Barycentric.X), AveragedSample, VertexWeightSums, OutTargets);
		AccumulateSurfaceSampleToVertex(SurfaceSample.VertexIndex1, static_cast<double>(SurfaceSample.Barycentric.Y), AveragedSample, VertexWeightSums, OutTargets);
		AccumulateSurfaceSampleToVertex(SurfaceSample.VertexIndex2, static_cast<double>(SurfaceSample.Barycentric.Z), AveragedSample, VertexWeightSums, OutTargets);
	}

	const uint32 MeshHash = GetTypeHash(AssocAsset->GetPathName());
	for (int32 VertexIndex = 0; VertexIndex < AssocVertexCount; ++VertexIndex)
	{
		if (VertexWeightSums[VertexIndex] > 1e-6)
		{
			const double InvWeight = 1.0 / VertexWeightSums[VertexIndex];
			OutTargets.R[VertexIndex] *= InvWeight;
			OutTargets.G[VertexIndex] *= InvWeight;
			OutTargets.B[VertexIndex] *= InvWeight;
		}
		else
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			BuildVertexTargetsSynthetic(MeshHash, VertexIndex, R, G, B);
			OutTargets.R[VertexIndex] = R;
			OutTargets.G[VertexIndex] = G;
			OutTargets.B[VertexIndex] = B;
			OutTargets.Stats.FallbackVertexCount += 1;
		}
	}

	return OutTargets.bHasAnyRealSample;
}

static bool BuildPrecomputedVolumeTargets(const UWishGIMeshAssocAsset* AssocAsset, const FTargetContext& TargetContext, FVertexTargets& OutTargets)
{
	if (!AssocAsset || !TargetContext.World)
	{
		return false;
	}

	const int32 AssocVertexCount = AssocAsset->VertexAssociations.Num();
	const auto BuildAllSyntheticFallback = [&AssocAsset, &OutTargets, AssocVertexCount]()
	{
		BuildSyntheticTargets(AssocAsset, OutTargets);
		OutTargets.Stats.FallbackVertexCount += AssocVertexCount;
		return true;
	};

	if (BuildPrecomputedVolumeTargetsFromSurfaceSamples(AssocAsset, TargetContext, OutTargets))
	{
		return true;
	}

	TArray<FVector> LocalPositions;
	TArray<FVector> LocalNormals;
	if (!ExtractMeshVertexData(AssocAsset, LocalPositions, LocalNormals))
	{
		UE_LOG(LogWishGIBakeScene, Warning, TEXT("Failed to extract mesh vertex data for '%s', fallback to synthetic targets."), *AssocAsset->GetPathName());
		return BuildAllSyntheticFallback();
	}

	const UStaticMesh* SourceMesh = AssocAsset->SourceMesh.LoadSynchronous();
	TArray<FTransform> InstanceTransforms;
	GatherMeshInstanceTransforms(TargetContext.World, SourceMesh, InstanceTransforms);
	if (InstanceTransforms.Num() == 0)
	{
		UE_LOG(LogWishGIBakeScene, Warning, TEXT("No static mesh instances found in map for '%s', fallback to synthetic targets."), *AssocAsset->GetPathName());
		return BuildAllSyntheticFallback();
	}

	const int32 MeshVertexCount = FMath::Min(AssocVertexCount, FMath::Min(LocalPositions.Num(), LocalNormals.Num()));
	if (MeshVertexCount <= 0)
	{
		UE_LOG(LogWishGIBakeScene, Warning, TEXT("Mesh vertex data is empty for '%s', fallback to synthetic targets."), *AssocAsset->GetPathName());
		return BuildAllSyntheticFallback();
	}

	OutTargets.R.Init(0.0, AssocVertexCount);
	OutTargets.G.Init(0.0, AssocVertexCount);
	OutTargets.B.Init(0.0, AssocVertexCount);

	const uint32 MeshHash = GetTypeHash(AssocAsset->GetPathName());
	for (int32 VertexIndex = 0; VertexIndex < MeshVertexCount; ++VertexIndex)
	{
		double AccR = 0.0;
		double AccG = 0.0;
		double AccB = 0.0;
		int32 ValidSamplesForVertex = 0;

		for (const FTransform& Transform : InstanceTransforms)
		{
			const FVector WorldPosition = Transform.TransformPosition(LocalPositions[VertexIndex]);
			FVector WorldNormal = Transform.TransformVectorNoScale(LocalNormals[VertexIndex]).GetSafeNormal();
			if (WorldNormal.IsNearlyZero())
			{
				WorldNormal = FVector::UpVector;
			}
			OutTargets.Stats.QueryCount += 1;

			FSHVectorRGB3 Incident;
			if (!SampleIncidentRadianceAt(TargetContext, WorldPosition, Incident))
			{
				continue;
			}

			OutTargets.Stats.ValidCount += 1;
			const FLinearColor Sampled = IntegrateEffectiveDirections(Incident, WorldNormal, TargetContext.DirectionSamples);
			AccR += Sampled.R;
			AccG += Sampled.G;
			AccB += Sampled.B;
			ValidSamplesForVertex += 1;
		}

		if (ValidSamplesForVertex > 0)
		{
			OutTargets.R[VertexIndex] = AccR / static_cast<double>(ValidSamplesForVertex);
			OutTargets.G[VertexIndex] = AccG / static_cast<double>(ValidSamplesForVertex);
			OutTargets.B[VertexIndex] = AccB / static_cast<double>(ValidSamplesForVertex);
			OutTargets.bHasAnyRealSample = true;
		}
		else
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			BuildVertexTargetsSynthetic(MeshHash, VertexIndex, R, G, B);
			OutTargets.R[VertexIndex] = R;
			OutTargets.G[VertexIndex] = G;
			OutTargets.B[VertexIndex] = B;
			OutTargets.Stats.FallbackVertexCount += 1;
		}
	}

	for (int32 VertexIndex = MeshVertexCount; VertexIndex < AssocVertexCount; ++VertexIndex)
	{
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		BuildVertexTargetsSynthetic(MeshHash, VertexIndex, R, G, B);
		OutTargets.R[VertexIndex] = R;
		OutTargets.G[VertexIndex] = G;
		OutTargets.B[VertexIndex] = B;
		OutTargets.Stats.FallbackVertexCount += 1;
	}

	if (!OutTargets.bHasAnyRealSample)
	{
		UE_LOG(LogWishGIBakeScene, Warning, TEXT("No valid precomputed lighting samples found for '%s', used synthetic fallback for all vertices."), *AssocAsset->GetPathName());
	}

	return true;
}

static bool BuildTargetsForMesh(const UWishGIMeshAssocAsset* AssocAsset, const FTargetContext& TargetContext, FVertexTargets& OutTargets)
{
	OutTargets = FVertexTargets();

	switch (TargetContext.Source)
	{
	case ETargetSource::Synthetic:
		BuildSyntheticTargets(AssocAsset, OutTargets);
		return true;
	case ETargetSource::PrecomputedVolume:
		return BuildPrecomputedVolumeTargets(AssocAsset, TargetContext, OutTargets);
	case ETargetSource::RayTrace:
	default:
		return false;
	}
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

static double DotArray(const TArray<double>& A, const TArray<double>& B)
{
	double Sum = 0.0;
	const int32 Num = FMath::Min(A.Num(), B.Num());
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Sum += A[Index] * B[Index];
	}
	return Sum;
}

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
	double RsOld = DotArray(R, R);
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
		const double Denom = DotArray(P, Ap);
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

		const double RsNew = DotArray(R, R);
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

static void BuildSystemsFromSurfaceSamples(
	const UWishGIMeshAssocAsset* AssocAsset,
	const FSurfaceSampleTargets& Targets,
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

	const int32 SampleCount = FMath::Min(AssocAsset ? AssocAsset->SurfaceSamples.Num() : 0, Targets.ValidMask.Num());
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		if (Targets.ValidMask[SampleIndex] == 0)
		{
			continue;
		}

		const FWishGISurfaceSample& SurfaceSample = AssocAsset->SurfaceSamples[SampleIndex];
		TMap<int32, double> SampleWeights;

		if (AssocAsset->VertexAssociations.IsValidIndex(SurfaceSample.VertexIndex0))
		{
			AddAssociationWeight(AssocAsset->VertexAssociations[SurfaceSample.VertexIndex0], static_cast<double>(SurfaceSample.Barycentric.X), MeshProbeCount, SampleWeights);
		}
		if (AssocAsset->VertexAssociations.IsValidIndex(SurfaceSample.VertexIndex1))
		{
			AddAssociationWeight(AssocAsset->VertexAssociations[SurfaceSample.VertexIndex1], static_cast<double>(SurfaceSample.Barycentric.Y), MeshProbeCount, SampleWeights);
		}
		if (AssocAsset->VertexAssociations.IsValidIndex(SurfaceSample.VertexIndex2))
		{
			AddAssociationWeight(AssocAsset->VertexAssociations[SurfaceSample.VertexIndex2], static_cast<double>(SurfaceSample.Barycentric.Z), MeshProbeCount, SampleWeights);
		}

		AccumulateSampleConstraint(SampleWeights, Targets.R[SampleIndex], Lambda, OutSystemR);
		AccumulateSampleConstraint(SampleWeights, Targets.G[SampleIndex], Lambda, OutSystemG);
		AccumulateSampleConstraint(SampleWeights, Targets.B[SampleIndex], Lambda, OutSystemB);
	}

	const double DiagEpsilon = 1e-4 + static_cast<double>(Lambda) * 1e-3;
	for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
	{
		At(OutSystemR, ProbeIndex, ProbeIndex) += DiagEpsilon;
		At(OutSystemG, ProbeIndex, ProbeIndex) += DiagEpsilon;
		At(OutSystemB, ProbeIndex, ProbeIndex) += DiagEpsilon;
	}
}
static void BuildSystems(
	const UWishGIMeshAssocAsset* AssocAsset,
	const FVertexTargets& Targets,
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

		const double TargetR = Targets.R.IsValidIndex(VertexIndex) ? Targets.R[VertexIndex] : 0.0;
		const double TargetG = Targets.G.IsValidIndex(VertexIndex) ? Targets.G[VertexIndex] : 0.0;
		const double TargetB = Targets.B.IsValidIndex(VertexIndex) ? Targets.B[VertexIndex] : 0.0;

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

static bool SolveProbeSignals(
	const UWishGIMeshAssocAsset* AssocAsset,
	int32 MeshProbeCount,
	float Lambda,
	const FTargetContext& TargetContext,
	FSolvedSignals& OutResult,
	FTargetStats& OutTargetStats)
{
	OutResult = FSolvedSignals();
	OutTargetStats = FTargetStats();
	if (!AssocAsset || MeshProbeCount <= 0)
	{
		return false;
	}

	FLinearSystemDense SystemR;
	FLinearSystemDense SystemG;
	FLinearSystemDense SystemB;

	bool bBuiltSystemFromSurfaceSamples = false;
	if (AssocAsset->SurfaceSamples.Num() > 0)
	{
		FSurfaceSampleTargets SurfaceTargets;
		bool bBuiltTargets = false;
		if (TargetContext.Source == ETargetSource::PrecomputedVolume)
		{
			bBuiltTargets = BuildSurfaceSampleTargetsFromPrecomputedVolume(AssocAsset, TargetContext, SurfaceTargets);
		}
		else if (TargetContext.Source == ETargetSource::RayTrace)
		{
			bBuiltTargets = BuildSurfaceSampleTargetsFromRayTrace(AssocAsset, TargetContext, SurfaceTargets);
		}

		if (bBuiltTargets)
		{
			OutTargetStats = SurfaceTargets.Stats;
			BuildSystemsFromSurfaceSamples(AssocAsset, SurfaceTargets, MeshProbeCount, Lambda, SystemR, SystemG, SystemB);
			bBuiltSystemFromSurfaceSamples = true;
		}
	}

	if (!bBuiltSystemFromSurfaceSamples)
	{
		FVertexTargets Targets;
		if (!BuildTargetsForMesh(AssocAsset, TargetContext, Targets))
		{
			return false;
		}
		OutTargetStats = Targets.Stats;
		BuildSystems(AssocAsset, Targets, MeshProbeCount, Lambda, SystemR, SystemG, SystemB);
	}

	const int32 MaxIterations = FMath::Clamp(MeshProbeCount * 4, 32, 512);
	const double Tolerance = 1e-5;

	TArray<double> XR;
	TArray<double> XG;
	TArray<double> XB;

	OutResult.StatsR = SolveByConjugateGradient(SystemR, XR, MaxIterations, Tolerance);
	OutResult.StatsG = SolveByConjugateGradient(SystemG, XG, MaxIterations, Tolerance);
	OutResult.StatsB = SolveByConjugateGradient(SystemB, XB, MaxIterations, Tolerance);

	OutResult.R.SetNum(MeshProbeCount);
	OutResult.G.SetNum(MeshProbeCount);
	OutResult.B.SetNum(MeshProbeCount);

	for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
	{
		const double RV = XR.IsValidIndex(ProbeIndex) ? XR[ProbeIndex] : 0.0;
		const double GV = XG.IsValidIndex(ProbeIndex) ? XG[ProbeIndex] : 0.0;
		const double BV = XB.IsValidIndex(ProbeIndex) ? XB[ProbeIndex] : 0.0;
		OutResult.R[ProbeIndex] = static_cast<float>(FMath::Clamp(RV, 0.0, 1.0));
		OutResult.G[ProbeIndex] = static_cast<float>(FMath::Clamp(GV, 0.0, 1.0));
		OutResult.B[ProbeIndex] = static_cast<float>(FMath::Clamp(BV, 0.0, 1.0));
	}

	return true;
}

static FLinearColor BuildTint(uint32 Hash)
{
	const float R = 0.75f + 0.25f * static_cast<float>(HashToUnit(HashCombine(Hash, 17u)));
	const float G = 0.75f + 0.25f * static_cast<float>(HashToUnit(HashCombine(Hash, 29u)));
	const float B = 0.75f + 0.25f * static_cast<float>(HashToUnit(HashCombine(Hash, 43u)));
	return FLinearColor(R, G, B, 1.0f);
}

static FWishGIProbeSHRecord BuildProbeRecord(float SignalR, float SignalG, float SignalB, int32 SHOrder, uint32 MeshHash, int32 ProbeIndex)
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
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Map=/Game/Maps/YourMap (required when -TargetSource=PrecomputedVolume or RayTrace)"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -AssocPath=/Game/WishGI/MeshAssoc"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -OutPath=/Game/WishGI/Bake"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -AssetName=WishGI_ProbeMap"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -TargetSource=Synthetic|PrecomputedVolume|Hybrid|RayTrace"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -PrecomputedSource=Auto|VLM|Sparse (used when -TargetSource=PrecomputedVolume)"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -SHOrder=2"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Directions=192"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Lambda=0.1"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Overwrite"));
	UE_LOG(LogWishGIBakeScene, Display, TEXT("  -Help"));
}
int32 UWishGIBakeSceneCommandlet::Main(const FString& Params)
{
	WishGIBakeScene::FSettings Settings;
	FParse::Value(*Params, TEXT("Map="), Settings.MapPath);
	FParse::Value(*Params, TEXT("AssocPath="), Settings.AssocPath);
	FParse::Value(*Params, TEXT("OutPath="), Settings.OutPath);
	FParse::Value(*Params, TEXT("AssetName="), Settings.AssetName);
	FParse::Value(*Params, TEXT("TargetSource="), Settings.TargetSourceName);
	FParse::Value(*Params, TEXT("PrecomputedSource="), Settings.PrecomputedSourceName);
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

	if (!WishGIBakeScene::ParseTargetSource(Settings.TargetSourceName, Settings.TargetSource))
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Invalid TargetSource '%s'."), *Settings.TargetSourceName);
		return 1;
	}

	if (!WishGIBakeScene::ParsePrecomputedSource(Settings.PrecomputedSourceName, Settings.PrecomputedSource))
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Invalid PrecomputedSource '%s'."), *Settings.PrecomputedSourceName);
		return 1;
	}

	if (Settings.TargetSource == WishGIBakeScene::ETargetSource::Hybrid)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("TargetSource '%s' is reserved but not implemented yet. Use Synthetic, PrecomputedVolume, or RayTrace."), *WishGIBakeScene::TargetSourceToString(Settings.TargetSource));
		return 1;
	}

	Settings.SHOrder = FMath::Clamp(Settings.SHOrder, 2, 3);
	Settings.Directions = FMath::Max(1, Settings.Directions);
	Settings.Lambda = FMath::Clamp(Settings.Lambda, 0.0f, 10.0f);

	WishGIBakeScene::FTargetContext TargetContext;
	TargetContext.Source = Settings.TargetSource;
	TargetContext.PrecomputedSource = Settings.PrecomputedSource;
	WishGIBakeScene::BuildDirectionSamples(Settings.Directions, TargetContext.DirectionSamples);

	if (Settings.TargetSource == WishGIBakeScene::ETargetSource::PrecomputedVolume || Settings.TargetSource == WishGIBakeScene::ETargetSource::RayTrace)
	{
		if (Settings.MapPath.IsEmpty())
		{
			UE_LOG(LogWishGIBakeScene, Error, TEXT("-Map is required when -TargetSource=%s."), *WishGIBakeScene::TargetSourceToString(Settings.TargetSource));
			return 2;
		}

		TargetContext.World = WishGIBakeScene::LoadWorldForSampling(Settings.MapPath);
		if (!TargetContext.World)
		{
			UE_LOG(LogWishGIBakeScene, Error, TEXT("Failed to load map '%s' for %s sampling."), *Settings.MapPath, *WishGIBakeScene::TargetSourceToString(Settings.TargetSource));
			return 2;
		}

		if (Settings.TargetSource == WishGIBakeScene::ETargetSource::PrecomputedVolume)
		{
			WishGIBakeScene::LogPrecomputedDataSummary(TargetContext.World);
		}
		else if (Settings.TargetSource == WishGIBakeScene::ETargetSource::RayTrace)
		{
			WishGIBakeScene::GatherRayTraceLights(TargetContext.World, TargetContext);
		}
	}

	TArray<UWishGIMeshAssocAsset*> AssocAssets;
	WishGIBakeScene::GatherAssocAssets(Settings.AssocPath, AssocAssets);
	if (AssocAssets.Num() == 0)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("No mesh association assets found at '%s'."), *Settings.AssocPath);
		return 3;
	}

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
		return 4;
	}

	ProbeMapAsset->SourceMapPath = Settings.MapPath;
	ProbeMapAsset->TargetSource = WishGIBakeScene::TargetSourceToString(Settings.TargetSource);
	ProbeMapAsset->SHOrder = Settings.SHOrder;
	ProbeMapAsset->DirectionCount = Settings.Directions;
	ProbeMapAsset->Lambda = Settings.Lambda;
	ProbeMapAsset->ProbeMapTexture.Reset();
	ProbeMapAsset->ProbeRecords.Reset();
	ProbeMapAsset->MeshRanges.Reset();
	ProbeMapAsset->SolverIterations = 0;
	ProbeMapAsset->SolverResidual = 0.0f;
	ProbeMapAsset->RealSampleQueryCount = 0;
	ProbeMapAsset->RealSampleValidCount = 0;
	ProbeMapAsset->RealSampleValidRatio = 0.0f;

	int32 RunningProbeStart = 0;
	double ResidualAccum = 0.0;
	int32 IterationAccum = 0;
	int32 SolvedChannelCount = 0;
	int32 SolvedMeshCount = 0;
	int32 SkippedMeshCount = 0;

	int32 RealSampleQueryAccum = 0;
	int32 RealSampleValidAccum = 0;
	int32 RealSampleFallbackVertexAccum = 0;

	for (UWishGIMeshAssocAsset* AssocAsset : AssocAssets)
	{
		if (!AssocAsset)
		{
			continue;
		}

		const int32 MeshProbeCount = FMath::Clamp(AssocAsset->ProbeCount, 1, 256);
		WishGIBakeScene::FSolvedSignals Signals;
		WishGIBakeScene::FTargetStats TargetStats;
		if (!WishGIBakeScene::SolveProbeSignals(AssocAsset, MeshProbeCount, Settings.Lambda, TargetContext, Signals, TargetStats))
		{
			++SkippedMeshCount;
			UE_LOG(LogWishGIBakeScene, Warning, TEXT("Skipping '%s': failed to build targets/solve."), *AssocAsset->GetPathName());
			continue;
		}

		RealSampleQueryAccum += TargetStats.QueryCount;
		RealSampleValidAccum += TargetStats.ValidCount;
		RealSampleFallbackVertexAccum += TargetStats.FallbackVertexCount;

		FWishGIProbeMeshRange MeshRange;
		MeshRange.SourceMesh = AssocAsset->SourceMesh;
		MeshRange.ProbeStart = RunningProbeStart;
		MeshRange.ProbeCount = MeshProbeCount;
		ProbeMapAsset->MeshRanges.Add(MeshRange);

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

		const uint32 MeshHash = GetTypeHash(AssocAsset->GetPathName());
		for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
		{
			const float R = Signals.R.IsValidIndex(ProbeIndex) ? Signals.R[ProbeIndex] : 0.0f;
			const float G = Signals.G.IsValidIndex(ProbeIndex) ? Signals.G[ProbeIndex] : 0.0f;
			const float B = Signals.B.IsValidIndex(ProbeIndex) ? Signals.B[ProbeIndex] : 0.0f;
			ProbeMapAsset->ProbeRecords.Add(WishGIBakeScene::BuildProbeRecord(R, G, B, Settings.SHOrder, MeshHash, ProbeIndex));
		}

		RunningProbeStart += MeshProbeCount;
		++SolvedMeshCount;
	}

	if (SolvedMeshCount <= 0 || RunningProbeStart <= 0)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("No mesh was solved. Check TargetSource and map lighting data."));
		return 5;
	}

	ProbeMapAsset->TotalProbeCount = RunningProbeStart;
	ProbeMapAsset->SuggestedProbeMapSize = WishGIBakeScene::ComputeProbeMapSize(ProbeMapAsset->TotalProbeCount, Settings.SHOrder);
	ProbeMapAsset->RealSampleQueryCount = RealSampleQueryAccum;
	ProbeMapAsset->RealSampleValidCount = RealSampleValidAccum;
	ProbeMapAsset->RealSampleValidRatio = (RealSampleQueryAccum > 0)
		? static_cast<float>(static_cast<double>(RealSampleValidAccum) / static_cast<double>(RealSampleQueryAccum))
		: 0.0f;

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
		return 6;
	}

	UE_LOG(LogWishGIBakeScene, Display, TEXT("Bake complete: '%s'"), *ObjectPath);
	UE_LOG(LogWishGIBakeScene, Display, TEXT("TargetSource=%s, AssocAssets=%d, SolvedMeshes=%d, SkippedMeshes=%d, TotalProbes=%d, SHOrder=%d, SuggestedProbeMap=%dx%d, AvgSolverIter=%d, AvgResidual=%.6f"),
		*WishGIBakeScene::TargetSourceToString(Settings.TargetSource),
		AssocAssets.Num(),
		SolvedMeshCount,
		SkippedMeshCount,
		ProbeMapAsset->TotalProbeCount,
		ProbeMapAsset->SHOrder,
		ProbeMapAsset->SuggestedProbeMapSize.X,
		ProbeMapAsset->SuggestedProbeMapSize.Y,
		ProbeMapAsset->SolverIterations,
		ProbeMapAsset->SolverResidual);

	if (Settings.TargetSource == WishGIBakeScene::ETargetSource::PrecomputedVolume)
	{
		UE_LOG(LogWishGIBakeScene, Display, TEXT("PrecomputedSource=%s"), *WishGIBakeScene::PrecomputedSourceToString(Settings.PrecomputedSource));
	}

	if (Settings.TargetSource == WishGIBakeScene::ETargetSource::PrecomputedVolume || Settings.TargetSource == WishGIBakeScene::ETargetSource::RayTrace)
	{
		UE_LOG(LogWishGIBakeScene, Display, TEXT("Real sampling stats: Query=%d, Valid=%d, ValidRatio=%.3f, FallbackVertices=%d"),
			RealSampleQueryAccum,
			RealSampleValidAccum,
			ProbeMapAsset->RealSampleValidRatio,
			RealSampleFallbackVertexAccum);
	}

	return 0;
}

















