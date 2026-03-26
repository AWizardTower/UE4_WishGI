#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WishGIMeshAssocAsset.generated.h"

class UStaticMesh;

USTRUCT(BlueprintType)
struct FWishGIProbeVertexAssociation
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	uint8 ProbeIndex0 = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	uint8 ProbeIndex1 = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	uint8 Weight0 = 255;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	uint8 Weight1 = 0;
};

UCLASS(BlueprintType)
class WISHGIEDITOR_API UWishGIMeshAssocAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TSoftObjectPtr<UStaticMesh> SourceMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 LODIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 VertexCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 ProbeCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	float SampleDensity = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 AssociationsPerVertex = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 GeneratedSampleCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 KMeansIterations = 8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	int32 RandomSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WishGI")
	TArray<FWishGIProbeVertexAssociation> VertexAssociations;
};
