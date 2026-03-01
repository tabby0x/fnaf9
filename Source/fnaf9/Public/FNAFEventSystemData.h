#pragma once
#include "CoreMinimal.h"
#include "FNAFEventSystemData.generated.h"

USTRUCT(BlueprintType)
struct FNAF9_API FFNAFEventSystemData {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame, meta = (AllowPrivateAccess = true))
    TSet<FName> EventsTriggered;

    FFNAFEventSystemData();
};