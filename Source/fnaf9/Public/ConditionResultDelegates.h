#pragma once
#include "CoreMinimal.h"
#include "OnConditionResultUpdatedDelegate.h"
#include "ConditionCheckInterface.h"
#include "ConditionResultDelegates.generated.h"

USTRUCT(BlueprintType)
struct FConditionResultDelegates {
    GENERATED_BODY()
public:
    UPROPERTY()
    TArray<FOnConditionResultUpdated> Delegates;

    FNAF9_API FConditionResultDelegates();

    void CallDelegates(const TScriptInterface<IConditionCheckInterface>& UpdatedCondition)
    {
        for (FOnConditionResultUpdated& Delegate : Delegates)
        {
            if (Delegate.IsBound())
            {
                Delegate.Execute(UpdatedCondition);
            }
        }
    }
};