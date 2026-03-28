#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WishGIProbeMapAsset.generated.h"

class UStaticMesh;
class UTexture2D;

USTRUCT(BlueprintType)
struct FWishGIProbeSHRecord
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	FLinearColor Pixel0 = FLinearColor::Black;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	FLinearColor Pixel1 = FLinearColor::Black;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TArray<FLinearColor> SHCoefficients;
};

USTRUCT(BlueprintType)
struct FWishGIProbeMeshRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TSoftObjectPtr<UStaticMesh> SourceMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 ProbeStart = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 ProbeCount = 0;
};

UCLASS(BlueprintType)
class WISHGIEDITOR_API UWishGIProbeMapAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	FString SourceMapPath;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	FString TargetSource = TEXT("Synthetic");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 SHOrder = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 DirectionCount = 192;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 SHCoefficientCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	float Lambda = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 TotalProbeCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 SolverIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	float SolverResidual = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 RealSampleQueryCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 RealSampleValidCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	float RealSampleValidRatio = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	FIntPoint SuggestedProbeMapSize = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TSoftObjectPtr<UTexture2D> ProbeMapTexture;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TArray<FWishGIProbeSHRecord> ProbeRecords;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TArray<FWishGIProbeMeshRange> MeshRanges;
};

