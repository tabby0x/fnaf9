#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OnLevelStreamingFinishedDelegate.h"
#include "StreamingLoadComponent.generated.h"

class ALevelStreamingVolume;

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API UStreamingLoadComponent : public UActorComponent {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnLevelStreamingFinished OnLevelStreamingFinished;

    UStreamingLoadComponent();

    UFUNCTION(BlueprintCallable)
    void StartAsyncLoadForLocation(const FVector& WorldLocation);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsStreamingLevels() const;

    UFUNCTION(BlueprintCallable)
    void EnableAllStreamingVolumes(bool bEnable);

private:
    /** Called as latent callback when each level finishes loading */
    UFUNCTION()
    void OnLevelLoaded();

    /** Kicks off loading the next level in LevelsToStream via LoadStreamLevel */
    void LoadNextLevel();

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bTurnVolumesOnAfterLoad;

    /** Level names collected from streaming volumes at the target location */
    UPROPERTY()
    TArray<FName> LevelsToStream;

    /** Index into LevelsToStream for the next level to load */
    UPROPERTY()
    int32 CurrentLevelIndex;

    /** True while an async load sequence is in progress */
    UPROPERTY()
    bool bIsStreamingLevels;
};