#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConditionCheckInterface.h"
#include "EConditionCheckType.h"
#include "OnConditionalCheckUpdatedDelegate.h"
#include "ConditionalCheckComponent.generated.h"

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API UConditionalCheckComponent : public UActorComponent, public IConditionCheckInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnConditionalCheckUpdated OnConditionalCheckUpdated;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EConditionCheckType ConditionCheck;

private:
    UPROPERTY(Transient)
    TArray<TScriptInterface<IConditionCheckInterface>> ConditionalComponents;

    UPROPERTY(Transient)
    bool bConditionStatus;

public:
    UConditionalCheckComponent();

    virtual void BeginPlay() override;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool GetConditionStatus() const { return bConditionStatus; }

    // IConditionCheckInterface
    virtual bool ConditionsMet_Implementation() override;

private:
    UFUNCTION()
    void OnConditionUpdated(TScriptInterface<IConditionCheckInterface> ConditionCheckInterface);
};