#include "WorldStateComponent.h"
#include "WorldStateSystem.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"

UWorldStateComponent::UWorldStateComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bSaveOnStateChange = true;
}

bool UWorldStateComponent::HasValidSaveName() const
{
    return SaveName != NAME_None;
}

bool UWorldStateComponent::GetObjectState() const
{
    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return false;
    }

    UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    return WSS && WSS->IsActivated(SaveName);
}

void UWorldStateComponent::SetObjectState(bool bEnabled)
{
    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return;
    }

    UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    if (!WSS || SaveName == NAME_None)
    {
        return;
    }

    if (bEnabled)
    {
        WSS->AddActivated(SaveName);
    }
    else
    {
        WSS->RemoveActivated(SaveName);
    }
}