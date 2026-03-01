#include "UpdateMissionComponent.h"
#include "FNAFMissionSystem.h"

UUpdateMissionComponent::UUpdateMissionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    MissionName = NAME_None;
    MissionStateIndex = 0;
    bShouldSave = false;
}

FName UUpdateMissionComponent::GetMissionName() const
{
    return MissionName;
}

/*
 * Two modes: bShouldComplete=true checks IsCompletedMission directly.
 * bShouldComplete=false returns true if InfoState >= MissionStateIndex or mission is completed.
 */
bool UUpdateMissionComponent::HasMetCondition() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return false;
    }

    UFNAFMissionSystem* MissionSystem = GameInstance->GetSubsystem<UFNAFMissionSystem>();
    if (!MissionSystem)
    {
        return false;
    }

    if (bShouldComplete)
    {
        return MissionSystem->IsCompletedMission(MissionName);
    }
    else
    {
        FFNAFMissionState MissionState;
        bool bValidMission = false;
        MissionSystem->GetMissionState(MissionName, MissionState, bValidMission);

        return bValidMission
            && (MissionState.InfoState >= MissionStateIndex
                || MissionSystem->IsCompletedMission(MissionName));
    }
}

bool UUpdateMissionComponent::IsMissionFinished() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return false;
    }

    UFNAFMissionSystem* MissionSystem = GameInstance->GetSubsystem<UFNAFMissionSystem>();
    if (!MissionSystem)
    {
        return false;
    }

    return MissionSystem->IsCompletedMission(MissionName);
}

void UUpdateMissionComponent::UpdateMission()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return;
    }

    UFNAFMissionSystem* MissionSystem = GameInstance->GetSubsystem<UFNAFMissionSystem>();
    if (!MissionSystem)
    {
        return;
    }

    if (bShouldComplete)
    {
        if (!MissionSystem->IsCompletedMission(MissionName))
        {
            MissionSystem->CompleteMission(MissionName);
        }
    }
    else
    {
        FFNAFMissionState MissionState;
        bool bValidMission = false;
        MissionSystem->GetMissionState(MissionName, MissionState, bValidMission);

        if (!bValidMission || MissionState.InfoState < MissionStateIndex)
        {
            MissionSystem->SetMissionInfoState(MissionName, MissionStateIndex);
        }
    }
}