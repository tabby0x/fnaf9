#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "OnLevelManagerLevelsUpdatedDelegate.h"
#include "FNAFLevelManager.generated.h"

class ALevelStreamingVolume;
class APawn;
class ULevelStreaming;
class ULightScenarioManager;
class USceneComponent;

USTRUCT()
struct FStreamingLevelArray
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<TWeakObjectPtr<ULevelStreaming>> Levels;
};

UCLASS(Blueprintable)
class FNAF9_API UFNAFLevelManager : public UTickableWorldSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnLevelManagerLevelsUpdated OnlevelsUpdated;

    UFNAFLevelManager();

    // USubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // UTickableWorldSubsystem interface
    virtual void Tick(float DeltaSeconds) override;
    virtual TStatId GetStatId() const override;

    UFUNCTION(BlueprintCallable)
    void UnregisterStreamingSource(USceneComponent* SceneComponent);

    UFUNCTION(BlueprintCallable)
    void UnregisterPawnStreamingSource(USceneComponent* SceneComponent);

    UFUNCTION(BlueprintCallable)
    void SetPlayerPawn(APawn* PlayerPawn);

    UFUNCTION(BlueprintCallable)
    void SetLevelStreamingEnable(bool bEnable);

    UFUNCTION(BlueprintCallable)
    void RemoveLevelsFromStreamingSource(const USceneComponent* StreamingSource);

    UFUNCTION(BlueprintCallable)
    void RemoveLevelArray(const TArray<ULevelStreaming*>& Levels);

    UFUNCTION(BlueprintCallable)
    void RemoveLevel(ULevelStreaming* LevelToUnload);

    UFUNCTION(BlueprintCallable)
    void RemoveAllLevels();

    UFUNCTION(BlueprintCallable)
    void RegisterStreamingSource(USceneComponent* SceneComponent);

    UFUNCTION(BlueprintCallable)
    void RegisterPawnStreamingSource(USceneComponent* SceneComponent);

    UFUNCTION(BlueprintCallable)
    bool IsLevelStreamingEnable();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<USceneComponent*> GetStreamingSources() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<FName> GetLevelNamesForComponent(const USceneComponent* SceneComponent) const;

    /** Gets the streaming levels associated with a given scene component from the ComponentToLevelMap */
    const FStreamingLevelArray& GetLevelsForComponent(const USceneComponent* SceneComponent) const;

    UFUNCTION(BlueprintCallable)
    void AddLevelToLoad(ULevelStreaming* LevelToLoad);

    UFUNCTION(BlueprintCallable)
    void AddLevelsFromStreamingSourceToLoad(const USceneComponent* StreamingSource);

    UFUNCTION(BlueprintCallable)
    void AddLevelArrayToLoad(const TArray<ULevelStreaming*>& LevelsToLoad);

private:
    /** Handles Tick logic when bStreamingEnabled is false (sequential/manual loading) */
    void TickSequentialMode();

    /** Handles Tick logic when bStreamingEnabled is true (volume-based streaming) */
    void TickStreamingMode();

    /** Levels queued for sequential loading (used when bStreamingEnabled is false) */
    UPROPERTY()
    TArray<ULevelStreaming*> LevelsToLoadSequential;

    /** Set of levels currently marked as loaded (SetShouldBeLoaded) */
    UPROPERTY()
    TSet<ULevelStreaming*> CurrentLoadedLevels;

    /** Set of levels currently marked as visible (SetShouldBeVisible) */
    UPROPERTY()
    TSet<ULevelStreaming*> CurrentVisibleLevels;

    /** Registered streaming source components that drive volume-based streaming */
    UPROPERTY()
    TArray<USceneComponent*> StreamingSources;

    /** Maps each streaming source component to the levels it can see. Updated every Tick in streaming mode. */
    UPROPERTY()
    TMap<USceneComponent*, FStreamingLevelArray> ComponentToLevelMap;

    /** Maps pawns to their registered streaming source component */
    UPROPERTY()
    TMap<APawn*, USceneComponent*> PawnComponentMap;

    /** The currently active player pawn for streaming */
    UPROPERTY()
    APawn* CurrentPlayerPawn;

    /** The streaming source component for the current player pawn */
    UPROPERTY()
    USceneComponent* CurrentPlayerComp;

    /** Cached reference to the light scenario manager subsystem */
    UPROPERTY()
    ULightScenarioManager* lightScenarioManager;

    /** When true, Tick uses volume-based streaming. When false, uses sequential loading queue. */
    UPROPERTY()
    bool bStreamingEnabled;
};