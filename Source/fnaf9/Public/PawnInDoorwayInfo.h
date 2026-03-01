#pragma once
#include "CoreMinimal.h"
#include "EDoorSide.h"
#include "PawnInDoorwayInfo.generated.h"

USTRUCT(BlueprintType)
struct FPawnInDoorwayInfo {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EDoorSide SideEntered;

    FNAF9_API FPawnInDoorwayInfo();
    FPawnInDoorwayInfo(EDoorSide InSide) : SideEntered(InSide) {}
};