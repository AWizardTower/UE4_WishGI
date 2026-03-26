#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "WishGIMeshPrepCommandlet.generated.h"

UCLASS()
class WISHGIEDITOR_API UWishGIMeshPrepCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UWishGIMeshPrepCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;

private:
	void PrintUsage() const;
};
