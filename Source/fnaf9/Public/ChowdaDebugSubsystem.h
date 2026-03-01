#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AnimatronicTypeData.h"
#include "EMapArea.h"
#include "ChowdaDebugSubsystem.generated.h"

class UDataTable;
class ULevelLoadSubsystem;

UCLASS(Blueprintable)
class FNAF9_API UChowdaDebugSubsystem : public UWorldSubsystem {
    GENERATED_BODY()
public:

    /** Currently visible streaming levels (copied into LevelsToUnload by debug skip) */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<FName> VisibleLevels;

    /** New area levels to load (copied into LevelsToLoad by debug skip) */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<FName> NewAreaLevels;

    /** Light scenario area index for the new area */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 NewLightScenario;

private:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    ULevelLoadSubsystem* LevelLoader;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    UDataTable* LevelSystemData;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FName CurrentLocation;

    // Removes all items from all areas, then awards items from
    // the target area's DependentAreas. Used by GoToThisArea.
    void AwardRelevantItems(FName AreaRowName);

public:
    UChowdaDebugSubsystem();

    UFUNCTION(BlueprintCallable)
    void SpawnDLCRabbit(TArray<FAnimatronicTypeData> TypesToAlertIn);

    UFUNCTION(BlueprintCallable)
    void GoToThisArea(EMapArea MapArea);
};
