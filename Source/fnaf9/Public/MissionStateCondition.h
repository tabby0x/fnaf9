#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConditionCheckInterface.h"
#include "ConditionResultDelegates.h"
#include "EMissionStatus.h"
#include "FNAFMissionInfo.h"
#include "FNAFMissionState.h"
#include "OnMissionStateConditionMetDelegateDelegate.h"
#include "MissionStateCondition.generated.h"

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API UMissionStateCondition : public UActorComponent, public IConditionCheckInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnMissionStateConditionMetDelegate OnMissionConditionMet;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnMissionStateConditionMetDelegate OnMissionConditionAlreadyMet;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnMissionStateConditionMetDelegate OnMissionConditionUnMet;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FName MissionName;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EMissionStatus MissionStatus;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 MinMissionState;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 MaxMissionState;

    // Discovered from IDA
    UPROPERTY()
    bool bConditionWasMet;

    UPROPERTY()
    FConditionResultDelegates Delegates;

    UMissionStateCondition();

    virtual void BeginPlay() override;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool HasMetConditions() const;

    // IConditionCheckInterface
    virtual bool ConditionsMet_Implementation() override;
    virtual void BindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated) override;
    virtual void UnbindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated) override;

private:
    UFUNCTION()
    void OnMissionUpdated(const FName& InMissionName, const FFNAFMissionState& MissionState, const FFNAFMissionInfo& MissionInfo);
};
