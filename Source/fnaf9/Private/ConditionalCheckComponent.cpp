#include "ConditionalCheckComponent.h"

UConditionalCheckComponent::UConditionalCheckComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    bAutoActivate = true;
    ConditionCheck = EConditionCheckType::None;
    bConditionStatus = false;
}

void UConditionalCheckComponent::BeginPlay()
{
    Super::BeginPlay();

    AActor* Owner = GetOwner();
    if (!Owner) return;

    // Get all components on the owner
    TArray<UActorComponent*> ActorComponents;
    Owner->GetComponents(ActorComponents);

    // Create delegate bound to OnConditionUpdated
    FOnConditionResultUpdated Delegate;
    Delegate.BindUFunction(this, FName(TEXT("OnConditionUpdated")));

    // Iterate components, find siblings implementing IConditionCheckInterface (but not ourselves)
    for (UActorComponent* Comp : ActorComponents)
    {
        // Skip other UConditionalCheckComponents
        if (Cast<UConditionalCheckComponent>(Comp))
        {
            continue;
        }

        // Check if component implements IConditionCheckInterface
        if (Comp && Comp->GetClass() && Comp->GetClass()->ImplementsInterface(UConditionCheckInterface::StaticClass()))
        {
            IConditionCheckInterface* Interface = Cast<IConditionCheckInterface>(Comp);
            if (!Interface)
            {
                continue;
            }

            // Check if not already in our list
            bool bAlreadyAdded = false;
            for (const TScriptInterface<IConditionCheckInterface>& Existing : ConditionalComponents)
            {
                if (Existing.GetObject() == Comp)
                {
                    bAlreadyAdded = true;
                    break;
                }
            }

            if (!bAlreadyAdded)
            {
                TScriptInterface<IConditionCheckInterface> ScriptInterface;
                ScriptInterface.SetObject(Comp);
                ScriptInterface.SetInterface(Interface);
                ConditionalComponents.Add(ScriptInterface);
            }

            // Bind our delegate to this condition component
            IConditionCheckInterface::Execute_BindConditionUpdatedDelegate(Comp, Delegate);
        }
    }

    // Evaluate initial condition status and broadcast
    bConditionStatus = ConditionsMet_Implementation();
    OnConditionalCheckUpdated.Broadcast(this, bConditionStatus);
}

bool UConditionalCheckComponent::ConditionsMet_Implementation()
{
    switch (ConditionCheck)
    {
    case EConditionCheckType::All:
    {
        // AND: all must be true. Empty = true.
        bool bResult = true;
        for (const TScriptInterface<IConditionCheckInterface>& Condition : ConditionalComponents)
        {
            if (Condition.GetObject())
            {
                bResult &= IConditionCheckInterface::Execute_ConditionsMet(Condition.GetObject());
            }
        }
        return bResult;
    }

    case EConditionCheckType::Any:
    {
        // OR: any can be true. Empty = true.
        if (ConditionalComponents.Num() == 0)
        {
            return true;
        }

        bool bResult = false;
        for (const TScriptInterface<IConditionCheckInterface>& Condition : ConditionalComponents)
        {
            if (Condition.GetObject())
            {
                bResult |= IConditionCheckInterface::Execute_ConditionsMet(Condition.GetObject());
            }
        }
        return bResult;
    }

    default:
        // None or unknown: always true
        return true;
    }
}

void UConditionalCheckComponent::OnConditionUpdated(TScriptInterface<IConditionCheckInterface> ConditionCheckInterface)
{
    bool bNewStatus = ConditionsMet_Implementation();

    if (bConditionStatus != bNewStatus)
    {
        OnConditionalCheckUpdated.Broadcast(this, bNewStatus);
    }

    bConditionStatus = bNewStatus;
}