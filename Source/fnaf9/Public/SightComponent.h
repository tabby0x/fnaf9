#pragma once
#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "ESightType.h"
#include "OnSightChangedDelegate.h"
#include "SightComponent.generated.h"

class AActor;
class AMoonmanSpawnPoint;
class UVisualSourceComponent;

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API USightComponent : public USceneComponent {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnSightChanged OnSightChanged;

private:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float SightAngle;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float StartDistance;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float EndDistance;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    ESightType SightType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float AspectRatio;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MinYaw;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MaxYaw;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MinPitch;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MaxPitch;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 ThetaSteps;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 PhiSteps;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    TArray<AActor*> VisibleActors;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    TArray<AMoonmanSpawnPoint*> VisibleMMActors;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bSightDetectionEnabled;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bShowVisionDebug;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<FName> IncludeTags;

    // IDA: *(_WORD *)&this->bWaitingOnResults = 0 — two consecutive bools
    bool bWaitingOnResults;
    bool bWaitingOnMoonManResults;

public:
    USightComponent();

    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION(BlueprintCallable)
    void SetSightType(ESightType InSightType);

    UFUNCTION(BlueprintCallable)
    void SetSightParams(float Angle, float MinDistance, float MaxDistance);

    UFUNCTION(BlueprintCallable)
    void SetSightEnabled(bool bEnable);

    UFUNCTION(BlueprintCallable)
    void SetSightAngle(float Angle);

    UFUNCTION(BlueprintCallable)
    void SetIncludeTags(const TArray<FName>& InIgnoreTags);

    UFUNCTION(BlueprintCallable)
    void SetFrustumTypeParams(float InMinYaw, float InMaxYaw, float InMinPitch, float InMaxPitch);

    UFUNCTION(BlueprintCallable)
    void SetEndDistance(float Distance);

    UFUNCTION(BlueprintCallable)
    void SetAspectRatio(float Ratio);

    UFUNCTION()
    void OnVisualQueryResults(const TArray<TWeakObjectPtr<UVisualSourceComponent>>& DetectedSources);

private:
    UFUNCTION(BlueprintCallable)
    void OnVisibleActorDestroyed(AActor* DestroyedActor);

public:
    UFUNCTION()
    void OnMoonManQueryResults(const TArray<TWeakObjectPtr<AMoonmanSpawnPoint>>& MoonManSpawnPoints);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    ESightType IsUsingConeSight() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsSightEnabled() const;

    UFUNCTION(BlueprintCallable)
    bool IsActorVisible(AActor* Actor);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<AActor*> GetVisibleActors() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetStartDistance() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetSightAngle() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetMinYaw() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetMinPitch() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetMaxYaw() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetMaxPitch() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<FName> GetIncludeTags() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    void GetFrustumTypeParams(float& OutMinYaw, float& OutMaxYaw, float& OutMinPitch, float& OutMaxPitch) const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetEndDistance() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetAspectRatio() const;

private:
    /** Shared logic for diffing detected actors against VisibleActors. */
    void ProcessDetectionResults(TArray<AActor*>& ActorsSpotted);

    /** Notify a VisualSourceComponent of detection state if owner has "Player" tag. */
    void NotifyVisualSource(AActor* Actor, const FVector& ObserverLocation, bool bVisible);
};