#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConditionCheckInterface.h"
#include "ConditionResultDelegates.h"
#include "EConditionFailReason.h"
#include "EPlayerPawnType.h"
#include "FNAFInventoryTableStruct.h"
#include "FNAFMessageTableStruct.h"
#include "InventoryConditionalComponent.generated.h"

UCLASS(Blueprintable, ClassGroup = Custom, meta = (BlueprintSpawnableComponent))
class FNAF9_API UInventoryConditionalComponent : public UActorComponent, public IConditionCheckInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FName RequiredInventoryItem;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 RequiredSecurityLevel;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool RequiresPartyPass;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EPlayerPawnType RequiresPawn;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EPlayerPawnType PawnTypeIgnoresConditions;

private:
    UPROPERTY()
    FConditionResultDelegates Delegates;

public:
    UInventoryConditionalComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable)
    void SetNewConditions(FName NewRequiredInventoryItem, int32 NewRequiredSecurityLevel, bool NewRequiresPartyPass);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    void HasMetConditions(bool& OutConditionsMet, EConditionFailReason& OutFailReason);

    // IConditionCheckInterface
    virtual bool ConditionsMet_Implementation() override;
    virtual void BindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated) override;
    virtual void UnbindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated) override;

private:
    UFUNCTION()
    void OnItemCollected(FName ItemName, FFNAFInventoryTableStruct InventoryTableStruct);

    UFUNCTION()
    void OnMessageCollected(FName ItemName, FFNAFMessageTableStruct MessageTableStruct);

    void NotifyDelegates();
};