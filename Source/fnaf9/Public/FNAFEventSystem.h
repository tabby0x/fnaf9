#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FNAFEventSystemData.h"
#include "SaveHandlerInterface.h"
#include "FNAFEventSystem.generated.h"

class UAudioComponent;
class UFNAFSaveData;

UCLASS(Blueprintable)
class FNAF9_API UFNAFEventSystem : public UGameInstanceSubsystem, public ISaveHandlerInterface {
    GENERATED_BODY()
public:
    UFNAFEventSystem();

    UFUNCTION(BlueprintCallable)
    void UnpauseEventSystem();

    UFUNCTION(BlueprintCallable)
    void StoreEventTriggered(FName EventTag);

    UFUNCTION(BlueprintCallable)
    void StopEventTimer();

    UFUNCTION(BlueprintCallable)
    void StartEventTimer();

    UFUNCTION(BlueprintCallable)
    void SetEventTimeSeconds(float MinTimeBetweenEvents, float MaxTimeBetweenEvents);

    UFUNCTION(BlueprintCallable)
    void SetEventActorWeight(float NewWeight);

    UFUNCTION(BlueprintCallable)
    void SetCurrentAudioComponent(UAudioComponent* EventSoundCue);

    UFUNCTION(BlueprintCallable)
    void PauseEventSystem();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool HasEventBeenTriggered(FName EventTag) const;

    // ISaveHandlerInterface
    virtual void OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject) override;
    virtual void OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject) override;

private:
    // Core event tracking data -- contains TSet<FName> of triggered events
    UPROPERTY(SaveGame)
    FFNAFEventSystemData SystemData;

    // Audio component used for ambient event sounds (positioned around camera)
    UPROPERTY()
    UAudioComponent* CurrentAudioComp;

    // Currently active event actor (implements IFNAFEventObject)
    UPROPERTY()
    AActor* CurrentEventActor;

    FTimerHandle NextEventTimer;
    FTimerHandle WaitForDoneTimer;

    bool bIsRunning;
    bool bIsPaused;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MinIdleTimeEventSeconds;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MaxIdleTimeEventSeconds;

    /* Probability that OnEventTimer picks an event actor vs ambient sound.
       Random roll in [0,1]: if roll <= WeightEventActors AND actors exist, pick actor event. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float WeightEventActors;

    // Distance range for random ambient sound placement around the camera
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MinDistanceSounds;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MaxDistanceSounds;

    // Main timer callback -- finds event actors, picks one or plays ambient sound
    UFUNCTION()
    void OnEventTimer();

    // Called when CurrentAudioComp finishes playing ambient sound
    UFUNCTION()
    void OnAudioFinished();

    // Polls CurrentEventActor each second to check if IsEventFinished
    UFUNCTION()
    void OnEventObjectTick();
};
