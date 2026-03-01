#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "ELightScenarioArea.h"
#include "ELightingScenario.h"
#include "OnLightScenarioChangeDelegate.h"
#include "OnLightScenarioChangeParamDelegate.h"
#include "SaveHandlerInterface.h"
#include "LightScenarioManager.generated.h"

class ALightStreamingVolume;
class ULevelStreaming;
class UFNAFSaveData;

/*
 * State machine states for area transitions within the same lighting scenario.
 * Cycles: LoadingMapBuildData -> MakingOldInvisible -> MakingNewVisible ->
 *         PropagatingChanges -> UnloadingOldLevel -> Idle
 */
enum class EChangeAreaState : uint8
{
    Idle = 0,
    LoadingMapBuildData = 1,
    MakingOldInvisible = 2,
    MakingNewVisible = 3,
    PropagatingChanges = 4,
    UnloadingOldLevel = 5
};

/*
 * World subsystem that manages lighting scenario sub-levels. Each area of the
 * Pizzaplex has lighting sub-levels for different scenarios (Day, Night, Dawn,
 * DLC). Handles loading/unloading the correct lighting level as the player
 * moves between areas, and transitioning between scenarios.
 *
 * The original binary uses several Steel Wool custom engine additions:
 *   - UWorld::SetLightingScenariosLevelMap() -- registers lighting scenario levels
 *   - UWorld::scenarioAreaChange -- bool flag on world for area transitions
 *   - UWorld::UseScenarioAreaChange(bool) -- sets the above flag
 *   - UWorld::SetScenarioLevelsInCommon() -- optimization for shared data
 *   - UWorld::PropagateLightingScenarioChangeInitial/Incremenatal()
 *
 * Since we don't have the custom engine, we emulate these:
 *   - SetLightingScenariosLevelMap -- not needed (data stays in Settings CDO)
 *   - scenarioAreaChange -- local bScenarioAreaChange member
 *   - PropagateLighting* -- PropagateLoadedLightingData() which re-initializes
 *     level rendering resources to force MapBuildData re-registration
 */
UCLASS(Blueprintable)
class FNAF9_API ULightScenarioManager : public UWorldSubsystem, public ISaveHandlerInterface, public FTickableGameObject {
    GENERATED_BODY()
public:
    ULightScenarioManager();

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override;
    virtual TStatId GetStatId() const override;

    void OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject);
    void OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject);

    UFUNCTION(BlueprintCallable)
    void SetInitialScenario(int32 Area, ELightingScenario Scenario);

    UFUNCTION(BlueprintCallable)
    void BeginLoadSequence();

    UFUNCTION(BlueprintCallable)
    void EndLoadSequence();

    UFUNCTION(BlueprintCallable)
    void ChangeScenario(ELightingScenario NewScenario, bool bUseFade);

    UFUNCTION(BlueprintCallable)
    void ChangeArea(int32 Area);

    UFUNCTION(BlueprintCallable)
    void UnloadArea();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsChangingScenario() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    ELightingScenario GetCurrentLightingScenario() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    int32 GetCurrentArea() const;

    UFUNCTION(BlueprintCallable)
    FName GetLevelNameFromAreaScenario(int32 Area, ELightingScenario Scenario);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    void GetLightScenarioAreaFromMap(const FName& MapName, ELightScenarioArea& OutArea, ELightingScenario& OutScenario) const;

    UFUNCTION(BlueprintCallable)
    void BindOnBeginScenarioChange(FOnLightScenarioChangeParam Delegate);

    UFUNCTION(BlueprintCallable)
    void UnbindOnBeginScenarioChange(FOnLightScenarioChangeParam Delegate);

    UFUNCTION(BlueprintCallable)
    void BindOnEndScenarioChange(FOnLightScenarioChangeParam Delegate);

    UFUNCTION(BlueprintCallable)
    void UnbindOnEndScenarioChange(FOnLightScenarioChangeParam Delegate);

    UPROPERTY()
    bool bIsChangingScenario;

private:
    UFUNCTION()
    void OnFadeoutFinished();

    UFUNCTION()
    void OnUnloadFinished();

    UFUNCTION()
    void OnLoadFinished();

    UFUNCTION()
    void OnChangeAreaLoadFinished();

    UFUNCTION()
    void OnChangeAreaUnloadFinished();

    UFUNCTION()
    void OnPropagate();

    /** Called every ~0.25s from Tick. Handles volume checks and area change state machine. */
    UFUNCTION()
    void OnTick();

    /*
     * Emulates Steel Wool's PropagateLightingScenarioChange by re-initializing
     * rendering resources on all loaded levels, forcing MapBuildData / Volumetric
     * Lightmap re-registration with the rendering system.
     */
    void PropagateLoadedLightingData();

    UPROPERTY()
    ELightingScenario CurrentLightingScenario;

    UPROPERTY()
    ELightingScenario OldLightingScenario;

    UPROPERTY()
    ELightingScenario LoadedLightingScenario;

    UPROPERTY()
    int32 ScenarioArea;

    UPROPERTY()
    int32 LoadedScenarioArea;

    UPROPERTY()
    FName CurrentLightScenarioLevel;

    UPROPERTY()
    FName OldLightingLevel;

    UPROPERTY()
    bool bIsWaitingOnLoad;

    UPROPERTY()
    bool bIsUsingFade;

    EChangeAreaState changeAreaState;

    UPROPERTY()
    ULevelStreaming* levelWaitingOn;

    UPROPERTY()
    FOnLightScenarioChange OnBeginScenarioChange;

    UPROPERTY()
    FOnLightScenarioChange OnEndScenarioChange;

    UPROPERTY()
    TArray<TWeakObjectPtr<ALightStreamingVolume>> LightStreamingVolumes;

    float NextTickTime;
    float NextPropagateTime;
    bool bVolumesGathered;

    /** Emulates World->scenarioAreaChange from Steel Wool's custom engine. */
    bool bScenarioAreaChange;
};
