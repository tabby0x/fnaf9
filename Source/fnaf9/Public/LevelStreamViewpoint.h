#pragma once
#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "OnViewpointLevelsLoadedDelegate.h"
#include "LevelStreamViewpoint.generated.h"

class ULevelStreaming;

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API ULevelStreamViewpoint : public USceneComponent {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnViewpointLevelsLoaded OnLevelsLoaded;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bStreamingEnable;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bEnableStreamOnActivePawn;

    ULevelStreamViewpoint();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable)
    void SetStreamingEnable(bool bEnable);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<FName> GetStreamingLevelNames() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool AnyLevelsLoaded() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool AllLevelsLoaded() const;

private:
    UFUNCTION()
    void OnLevelManagerUpdated();

    /** Gets the weak object pointer array of streaming levels for this component from the LevelManager */
    TArray<TWeakObjectPtr<ULevelStreaming>> GetStreamingLevels() const;

    /** Tracks whether the OnLevelsLoaded delegate has been broadcast (reset when streaming is toggled or on BeginPlay) */
    UPROPERTY()
    bool bLevelsLoadedSent;
};