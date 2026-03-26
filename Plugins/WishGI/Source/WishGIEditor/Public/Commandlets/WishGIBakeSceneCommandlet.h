#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "WishGIBakeSceneCommandlet.generated.h"

UCLASS()
class WISHGIEDITOR_API UWishGIBakeSceneCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UWishGIBakeSceneCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;

private:
	void PrintUsage() const;
};
