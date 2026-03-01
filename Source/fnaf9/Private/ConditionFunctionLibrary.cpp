#include "ConditionFunctionLibrary.h"
#include "ConditionCheckInterface.h"

void UConditionFunctionLibrary::RemoveConditionDelegate(FConditionResultDelegates& DelegateHandle, FOnConditionResultUpdated Delegate)
{
    int32 Index = DelegateHandle.Delegates.IndexOfByPredicate([&](const FOnConditionResultUpdated& Existing) {
        return Existing == Delegate;
        });

    if (Index != INDEX_NONE)
    {
        DelegateHandle.Delegates.RemoveAt(Index);
    }
}

void UConditionFunctionLibrary::HasMetComponentConditionsWithGet(
    const AActor* ActorToCheck,
    bool& bOutConditionsMet,
    TArray<TScriptInterface<IConditionCheckInterface>>& OutConditionsSucceeded,
    TArray<TScriptInterface<IConditionCheckInterface>>& OutConditionsFailed)
{
    bOutConditionsMet = true;

    TArray<UActorComponent*> Components;
    const_cast<AActor*>(ActorToCheck)->GetComponents(Components);

    for (UActorComponent* Comp : Components)
    {
        if (!Comp || !Comp->GetClass()) continue;

        if (Comp->GetClass()->ImplementsInterface(UConditionCheckInterface::StaticClass()))
        {
            bool bMet = IConditionCheckInterface::Execute_ConditionsMet(Comp);
            bOutConditionsMet &= bMet;

            TScriptInterface<IConditionCheckInterface> Interface;
            Interface.SetObject(Comp);
            Interface.SetInterface(static_cast<IConditionCheckInterface*>(
                Comp->GetInterfaceAddress(UConditionCheckInterface::StaticClass())));

            if (bMet)
            {
                OutConditionsSucceeded.Add(Interface);
            }
            else
            {
                OutConditionsFailed.Add(Interface);
            }
        }
    }
}

bool UConditionFunctionLibrary::HasMetComponentConditions(const AActor* ActorToCheck)
{
    bool bResult = true;

    TArray<UActorComponent*> Components;
    const_cast<AActor*>(ActorToCheck)->GetComponents(Components);

    for (UActorComponent* Comp : Components)
    {
        if (!Comp || !Comp->GetClass()) continue;

        if (Comp->GetClass()->ImplementsInterface(UConditionCheckInterface::StaticClass()))
        {
            bResult &= IConditionCheckInterface::Execute_ConditionsMet(Comp);
        }
    }

    return bResult;
}

bool UConditionFunctionLibrary::HasMetAnyComponentConditions(const AActor* ActorToCheck)
{
    TArray<UActorComponent*> Components;
    const_cast<AActor*>(ActorToCheck)->GetComponents(Components);

    for (UActorComponent* Comp : Components)
    {
        if (!Comp || !Comp->GetClass()) continue;

        if (Comp->GetClass()->ImplementsInterface(UConditionCheckInterface::StaticClass()))
        {
            if (IConditionCheckInterface::Execute_ConditionsMet(Comp))
            {
                return true;
            }
        }
    }

    return false;
}

void UConditionFunctionLibrary::CallConditionUpdate(
    TScriptInterface<IConditionCheckInterface> ConditionCheckInterface,
    const FConditionResultDelegates& DelegateHandle)
{
    // Re-wrap the interface (pseudocode validates ObjectPointer before using InterfacePointer)
    TScriptInterface<IConditionCheckInterface> Interface;
    Interface.SetObject(ConditionCheckInterface.GetObject());
    if (ConditionCheckInterface.GetObject())
    {
        Interface.SetInterface(ConditionCheckInterface.GetInterface());
    }

    // CallDelegates is non-const in our implementation, cast away const
    const_cast<FConditionResultDelegates&>(DelegateHandle).CallDelegates(Interface);
}

void UConditionFunctionLibrary::AddConditionDelegate(FConditionResultDelegates& DelegateHandle, FOnConditionResultUpdated Delegate)
{
    // Add if not already present
    int32 ExistingIndex = DelegateHandle.Delegates.IndexOfByPredicate([&](const FOnConditionResultUpdated& Existing) {
        return Existing == Delegate;
        });

    if (ExistingIndex == INDEX_NONE)
    {
        DelegateHandle.Delegates.Add(Delegate);
    }
}

UConditionFunctionLibrary::UConditionFunctionLibrary()
{
}