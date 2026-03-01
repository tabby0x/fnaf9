#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConditionCheckInterface.h"
#include "ConditionResultDelegates.h"
#include "OnObjectStateChangedDelegate.h"
#include "WorldStateHandlerComponent.generated.h"

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API UWorldStateHandlerComponent : public UActorComponent, public IConditionCheckInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnObjectStateChanged OnObjectStateChanged;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FName ObjectStateName;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bConditionMetOnStateSet;

    UWorldStateHandlerComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable)
    void SetObjectState(bool bEnable);

    UFUNCTION(BlueprintCallable, BlueprintNativeEvent)
    void OnObjectStateChangedEvent();
    virtual void OnObjectStateChangedEvent_Implementation();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool HasValidSaveName() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool GetObjectState() const;

    // IConditionCheckInterface
    virtual bool ConditionsMet_Implementation() override;
    virtual void BindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated) override;
    virtual void UnbindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated) override;

private:
    // Condition result delegates for ConditionalCheckComponent chain
    UPROPERTY()
    FConditionResultDelegates Delegates;

    // Re-entrancy guard: true while SetObjectState is executing,
    // prevents OnObjectStateChangedHandle from re-broadcasting
    bool bSetFromHere;

    // Bound to WorldStateSystem->OnObjectStateChanged in BeginPlay
    UFUNCTION()
    void OnObjectStateChangedHandle(FName ObjectName, bool ObjectState);
};