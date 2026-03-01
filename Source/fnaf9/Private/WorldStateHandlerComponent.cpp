#include "WorldStateHandlerComponent.h"
#include "WorldStateSystem.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"

UWorldStateHandlerComponent::UWorldStateHandlerComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bConditionMetOnStateSet = true;
    bSetFromHere = false;
}

void UWorldStateHandlerComponent::BeginPlay()
{
    UActorComponent::BeginPlay();

    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return;
    }

    UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    if (WSS)
    {
        WSS->OnObjectStateChanged.AddDynamic(this, &UWorldStateHandlerComponent::OnObjectStateChangedHandle);
    }
}

void UWorldStateHandlerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UWorld* World = GetWorld();
    if (World && World->GetGameInstance())
    {
        UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
        if (WSS)
        {
            WSS->OnObjectStateChanged.RemoveDynamic(this, &UWorldStateHandlerComponent::OnObjectStateChangedHandle);
        }
    }

    UActorComponent::EndPlay(EndPlayReason);
}

// Returns true when (IsActivated == bConditionMetOnStateSet)
bool UWorldStateHandlerComponent::ConditionsMet_Implementation()
{
    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return false;
    }

    UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    if (!WSS)
    {
        return false;
    }

    bool bIsActivated = WSS->IsActivated(ObjectStateName);
    return bIsActivated == bConditionMetOnStateSet;
}

bool UWorldStateHandlerComponent::GetObjectState() const
{
    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return false;
    }

    UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    return WSS && WSS->IsActivated(ObjectStateName);
}

bool UWorldStateHandlerComponent::HasValidSaveName() const
{
    return ObjectStateName != NAME_None;
}

void UWorldStateHandlerComponent::SetObjectState(bool bEnable)
{
    bSetFromHere = true;

    if (ObjectStateName != NAME_None)
    {
        UWorld* World = GetWorld();
        if (World && IsValid(World) && IsValid(World->GetGameInstance()))
        {
            UWorldStateSystem* WSS = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
            if (IsValid(WSS))
            {
                if (bEnable)
                {
                    WSS->AddActivated(ObjectStateName);
                }
                else
                {
                    WSS->RemoveActivated(ObjectStateName);
                }
            }
        }
    }

    bSetFromHere = false;
}

void UWorldStateHandlerComponent::OnObjectStateChangedHandle(FName ObjectName, bool ObjectState)
{
    if (!bSetFromHere && ObjectName == ObjectStateName)
    {
        OnObjectStateChanged.Broadcast(this, ObjectState);

        // Notify condition chain
        TScriptInterface<IConditionCheckInterface> Interface;
        Interface.SetObject(this);
        Interface.SetInterface(static_cast<IConditionCheckInterface*>(
            GetInterfaceAddress(UConditionCheckInterface::StaticClass())));
        Delegates.CallDelegates(Interface);
    }
}

void UWorldStateHandlerComponent::OnObjectStateChangedEvent_Implementation()
{
}

void UWorldStateHandlerComponent::BindConditionUpdatedDelegate_Implementation(
    const FOnConditionResultUpdated& OnConditionResultUpdated)
{
    int32 Index = Delegates.Delegates.IndexOfByPredicate([&](const FOnConditionResultUpdated& Existing) {
        return Existing == OnConditionResultUpdated;
        });

    if (Index == INDEX_NONE)
    {
        Delegates.Delegates.Add(OnConditionResultUpdated);
    }
}

void UWorldStateHandlerComponent::UnbindConditionUpdatedDelegate_Implementation(
    const FOnConditionResultUpdated& OnConditionResultUpdated)
{
    int32 Index = Delegates.Delegates.IndexOfByPredicate([&](const FOnConditionResultUpdated& Existing) {
        return Existing == OnConditionResultUpdated;
        });

    if (Index != INDEX_NONE)
    {
        Delegates.Delegates.RemoveAt(Index);
    }
}