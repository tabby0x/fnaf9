#include "MissionStateCondition.h"
#include "FNAFMissionSystem.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"

UMissionStateCondition::UMissionStateCondition()
{
    PrimaryComponentTick.bCanEverTick = false;
    MissionStatus = EMissionStatus::Active;
    MissionName = NAME_None;
    bConditionWasMet = false;
    MinMissionState = 0;
    MaxMissionState = 0;
}

/* Binds OnMissionUpdated to OnNewActiveMissionAdded, OnActiveMissionUpdated,
   and OnMissionCompleted delegates, then checks initial condition state. */
void UMissionStateCondition::BeginPlay()
{
    Super::BeginPlay();

    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return;
    }

    UFNAFMissionSystem* MissionSystem = World->GetGameInstance()->GetSubsystem<UFNAFMissionSystem>();
    if (!MissionSystem)
    {
        return;
    }

    MissionSystem->OnNewActiveMissionAdded.AddDynamic(this, &UMissionStateCondition::OnMissionUpdated);
    MissionSystem->OnActiveMissionUpdated.AddDynamic(this, &UMissionStateCondition::OnMissionUpdated);
    MissionSystem->OnMissionCompleted.AddDynamic(this, &UMissionStateCondition::OnMissionUpdated);

    if (HasMetConditions())
    {
        OnMissionConditionAlreadyMet.Broadcast();
        bConditionWasMet = true;
    }
}

/* For Active/Complete status: condition met if mission status matches.
   For None status: additionally requires InfoState within [MinMissionState, MaxMissionState]. */
bool UMissionStateCondition::HasMetConditions() const
{
    UWorld* World = GetWorld();
    if (!World || !World->GetGameInstance())
    {
        return false;
    }

    UFNAFMissionSystem* MissionSystem = World->GetGameInstance()->GetSubsystem<UFNAFMissionSystem>();
    if (!MissionSystem)
    {
        return false;
    }

    FFNAFMissionState MissionState;
    bool bMissionValid = false;
    MissionSystem->GetMissionState(MissionName, MissionState, bMissionValid);

    if (!bMissionValid)
    {
        return false;
    }

    if (MissionState.Status != MissionStatus)
    {
        return false;
    }

    // For Active/Complete, status match alone is sufficient
    if (MissionStatus != EMissionStatus::None)
    {
        return true;
    }

    return MissionState.InfoState >= MinMissionState && MissionState.InfoState <= MaxMissionState;
}

bool UMissionStateCondition::ConditionsMet_Implementation()
{
    return HasMetConditions();
}

/* Re-evaluates condition on any mission state change and fires the
   appropriate delegate (Met/UnMet). Also notifies condition watchers. */
void UMissionStateCondition::OnMissionUpdated(
    const FName& InMissionName,
    const FFNAFMissionState& MissionState,
    const FFNAFMissionInfo& MissionInfo)
{
    if (HasMetConditions())
    {
        OnMissionConditionMet.Broadcast();
        bConditionWasMet = true;
    }
    else if (bConditionWasMet)
    {
        OnMissionConditionUnMet.Broadcast();
        bConditionWasMet = false;
    }

    TScriptInterface<IConditionCheckInterface> Interface;
    Interface.SetObject(this);
    Interface.SetInterface(static_cast<IConditionCheckInterface*>(
        GetInterfaceAddress(UConditionCheckInterface::StaticClass())));
    Delegates.CallDelegates(Interface);
}

void UMissionStateCondition::BindConditionUpdatedDelegate_Implementation(
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

void UMissionStateCondition::UnbindConditionUpdatedDelegate_Implementation(
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
