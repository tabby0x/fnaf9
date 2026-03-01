#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EMapArea.h"
#include "LoadCompletedDelegateDelegate.h"
#include "SaveHandlerInterface.h"
#include "LevelLoadSubsystem.generated.h"

class UDataTable;
class ULevelStreaming;

UCLASS(Blueprintable)
class FNAF9_API ULevelLoadSubsystem : public UTickableWorldSubsystem, public ISaveHandlerInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FLoadCompletedDelegate LoadCompleted;

    ULevelLoadSubsystem();

    // UTickableWorldSubsystem interface
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    UFUNCTION(BlueprintCallable)
    void LoadTheNextArea(EMapArea MapArea);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsTickable() const;

    UFUNCTION(BlueprintCallable)
    TArray<FName> GetVisibleLevels();

    UFUNCTION(BlueprintCallable)
    EMapArea GetCurrentMapArea();

	// getters for private members, used by UChowdaDebugSubsystem's debug skip function
    FName GetTableRowName() const { return TableRowName; }

private:
    /** Looks up the DataTable row by name and configures the load/unload arrays */
    void FindNextAreaInTable(FName NextRowName);

    /** The ULevelStreaming* currently being processed in the Tick state machine */
    UPROPERTY()
    ULevelStreaming* CurrentLevel;

    /** The current DataTable row name being processed */
    UPROPERTY()
    FName TableRowName;

    /** Level names to load (from the DataTable LevelsToLoad column) */
    UPROPERTY()
    TArray<FName> LoadedLevels;

    /** Level names to unload (from the DataTable LevelsToUnload column) */
    UPROPERTY()
    TArray<FName> UnloadedLevels;

    /** Reserved array - initialized empty in constructor, present in binary layout */
    UPROPERTY()
    TArray<FName> WaitingLevelsToLoad;

    /** Reserved array - initialized empty in constructor, present in binary layout */
    UPROPERTY()
    TArray<FName> WaitingLevelsToUnload;

    /** True if another LoadTheNextArea was called while a load was already in progress */
    UPROPERTY()
    bool QueuingUpAnotherLoad;

    /** Current index into LoadedLevels during the loading phase */
    UPROPERTY()
    int32 LoadCount;

    /** Current index into UnloadedLevels during the unloading phase */
    UPROPERTY()
    int32 UnloadCount;

    /** True once all levels in UnloadedLevels have been fully unloaded */
    UPROPERTY()
    bool OldLevelsAreUnLoaded;

    /** True once all levels in LoadedLevels are visible */
    UPROPERTY()
    bool NextLevelsAreVisible;

    /** Master tick enable - set true when a load is in progress, false when complete */
    UPROPERTY()
    bool CanTick;

    /** The DataTable containing level system info (loaded from /Game/Data/LevelSystemData) */
    UPROPERTY()
    UDataTable* LevelSystemData;

    /** The light scenario area index to change to after unloading completes */
    UPROPERTY()
    int32 LightScenario;

    /** The current map area being loaded/active */
    UPROPERTY()
    EMapArea CurrentMapArea;
};