#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LevelLoaderComponent.generated.h"

class UChowdaDebugSubsystem;
class ULevelStreaming;
class ULightScenarioManager;

/*
 * Orchestrates area transitions by sequentially unloading old streaming levels
 * and loading new ones, one per tick. Coordinates with ULightScenarioManager
 * to trigger lighting area changes between the unload and load phases.
 *
 * Flow:
 *   1. Blueprint sets LevelsToUnload, LevelsToLoad, NextLightScenario
 *   2. Blueprint calls LoadTheNextArea() which enables tick
 *   3. Tick Phase 1: Each tick, hide + unload one level from LevelsToUnload.
 *      When all unloaded, call LightScenarioManager->ChangeArea(NextLightScenario)
 *   4. Tick Phase 2: Wait for LightScenarioManager to finish, then each tick
 *      load + show one level from LevelsToLoad
 *   5. When all loaded, disable tick
 */
UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API ULevelLoaderComponent : public UActorComponent {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool UnloadScenarioOnly;

private:
    UPROPERTY(Transient)
    UChowdaDebugSubsystem* ChowdaDebuger;

    UPROPERTY(Transient)
    ULevelStreaming* CurrentLevel;

    UPROPERTY(Transient)
    ULevelStreaming* LightingMap;

    UPROPERTY(Transient)
    ULightScenarioManager* LightScenarioManager;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<FName> LevelsToLoad;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<FName> LevelsToUnload;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 NextLightScenario;

    UPROPERTY()
    int32 LoadCount;

    UPROPERTY()
    int32 UnloadCount;

    UPROPERTY()
    bool OldLevelsAreUnLoaded;

    UPROPERTY()
    bool NextLevelsAreVisible;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FName LightMapToUnload;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FName LightMapToLoad;

public:
    ULevelLoaderComponent();

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION(BlueprintCallable)
    void UnLoadCurrentScenario();

    UFUNCTION(BlueprintCallable)
    int32 SetNextLightScenario(int32 Scenario);

    UFUNCTION(BlueprintCallable)
    TArray<FName> SetLevelsToUnLoad(TArray<FName> UnloadedLevels);

    UFUNCTION(BlueprintCallable)
    TArray<FName> SetLevelsToLoad(TArray<FName> LoadedLevels);

    UFUNCTION(BlueprintCallable)
    void LoadTheNextScenario();

    UFUNCTION(BlueprintCallable)
    void LoadTheNextArea();

    UFUNCTION(BlueprintCallable)
    TArray<FName> GetLevelsToLoad();

private:
    UFUNCTION(BlueprintCallable)
    void DebugSkipToNextArea();
};
