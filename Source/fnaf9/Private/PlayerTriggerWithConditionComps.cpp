#include "PlayerTriggerWithConditionComps.h"
#include "ConditionCheckInterface.h"

// Parent enables tick; condition-based triggers disable it since they only fire on initial overlap
APlayerTriggerWithConditionComps::APlayerTriggerWithConditionComps()
{
    PrimaryActorTick.bCanEverTick = false;
    ConditionCheck = EConditionCheckType::All;
}

/* Three modes based on ConditionCheck:
   All:  returns true only if ALL condition components pass
   Any:  returns true if ANY condition component passes
   None: always returns true (conditions bypassed) */
bool APlayerTriggerWithConditionComps::CanTrigger_Implementation() const
{
    switch (ConditionCheck)
    {
    case EConditionCheckType::None:
        return true;

    case EConditionCheckType::Any:
    {
        TArray<UActorComponent*> Components;
        GetComponents<UActorComponent>(Components);

        for (UActorComponent* Comp : Components)
        {
            if (Comp && Comp->GetClass()->ImplementsInterface(UConditionCheckInterface::StaticClass()))
            {
                if (IConditionCheckInterface::Execute_ConditionsMet(Comp))
                {
                    return true;
                }
            }
        }
        return false;
    }

    case EConditionCheckType::All:
    default:
    {
        TArray<UActorComponent*> Components;
        GetComponents<UActorComponent>(Components);

        bool bResult = true;
        for (UActorComponent* Comp : Components)
        {
            if (Comp && Comp->GetClass()->ImplementsInterface(UConditionCheckInterface::StaticClass()))
            {
                bResult &= IConditionCheckInterface::Execute_ConditionsMet(Comp);
            }
        }
        return bResult;
    }
    }
}
