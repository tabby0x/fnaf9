#include "MissionDisplayQueueLibrary.h"

TArray<FMissionDisplayUpdateInfo> UMissionDisplayQueueLibrary::MissionUpdateQueue;

UMissionDisplayQueueLibrary::UMissionDisplayQueueLibrary()
{
}

/*
 * Deduplicates before adding: skips if an entry with the same MissionName,
 * InfoState, and UpdateType already exists. Prevents the UI from showing
 * multiple identical notifications when rapid state changes occur before
 * the UI consumes them.
 */
void UMissionDisplayQueueLibrary::PushMissionUpdate(EMissionUpdateType UpdateType, const FName& MissionName, const FFNAFMissionInfo& MissionInfo, const FFNAFMissionState& MissionState)
{
    for (const FMissionDisplayUpdateInfo& Existing : MissionUpdateQueue)
    {
        if (Existing.MissionName == MissionName
            && Existing.MissionState.InfoState == MissionState.InfoState
            && Existing.UpdateType == UpdateType)
        {
            return;
        }
    }

    FMissionDisplayUpdateInfo NewEntry;
    NewEntry.UpdateType = UpdateType;
    NewEntry.MissionName = MissionName;
    NewEntry.MissionInfo = MissionInfo;
    NewEntry.MissionState = MissionState;
    MissionUpdateQueue.Add(NewEntry);
}

/* EMissionInQueue::HasMoreMissions = 0, NoMoreMissions = 1 */
void UMissionDisplayQueueLibrary::HasMissionUpdateInQueue(EMissionInQueue& MissionInQueue)
{
    MissionInQueue = (MissionUpdateQueue.Num() == 0)
        ? EMissionInQueue::NoMoreMissions
        : EMissionInQueue::HasMoreMissions;
}

FMissionDisplayUpdateInfo UMissionDisplayQueueLibrary::GetNextMissionUpdate()
{
    FMissionDisplayUpdateInfo Result;

    if (MissionUpdateQueue.Num() > 0)
    {
        Result = MissionUpdateQueue[0];
        MissionUpdateQueue.RemoveAt(0);
    }

    return Result;
}

void UMissionDisplayQueueLibrary::ClearMissionUpdateQueue()
{
    MissionUpdateQueue.Empty();
}
