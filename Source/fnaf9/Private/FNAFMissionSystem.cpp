#include "FNAFMissionSystem.h"
#include "FNAFGameInstanceBase.h"
#include "FNAFSaveData.h"
#include "MissionMarker.h"
#include "Engine/DataTable.h"

UFNAFMissionSystem::UFNAFMissionSystem()
{
    static ConstructorHelpers::FObjectFinder<UDataTable> MissionDataObject(
        TEXT("DataTable'/Game/Data/MissionDataTable.MissionDataTable'"));
    static ConstructorHelpers::FObjectFinder<UDataTable> MissionTaskObject(
        TEXT("DataTable'/Game/Data/MissionTaskDataTable.MissionTaskDataTable'"));

    if (MissionDataObject.Object)
    {
        MissionDataTable = MissionDataObject.Object;
    }
    if (MissionTaskObject.Object)
    {
        MissionTaskDataTable = MissionTaskObject.Object;
    }
}

FFNAFMissionState* UFNAFMissionSystem::FindMissionStateEntry(const FName& MissionName)
{
    return MissionStates.Find(MissionName);
}

const FFNAFMissionState* UFNAFMissionSystem::FindMissionStateEntry(const FName& MissionName) const
{
    return MissionStates.Find(MissionName);
}

void UFNAFMissionSystem::RegisterMarker(AMissionMarker* Marker)
{
    if (Marker)
    {
        MissionMarkers.Add(Marker);
    }
}

void UFNAFMissionSystem::UnregisterMarker(AMissionMarker* Marker)
{
    MissionMarkers.RemoveSingle(Marker);
}

bool UFNAFMissionSystem::ShouldFilterForSurvival(const FFNAFMissionInfo& MissionInfo) const
{
    return IsInSurvivalMode() && !MissionInfo.bValidForSurvival;
}

bool UFNAFMissionSystem::IsInSurvivalMode() const
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFGameInstanceBase* FNAFInstance = Cast<UFNAFGameInstanceBase>(GameInstance);
        if (FNAFInstance)
        {
            return FNAFInstance->GetCurrentGameType() == EFNAFGameType::ChowdaMode;
        }
    }
    return false;
}

void UFNAFMissionSystem::SetupNewGame()
{
    MissionStates.Empty();
    MissionMarkers.Empty();
}

void UFNAFMissionSystem::GetMissionInfo(const FName& MissionName, FFNAFMissionInfo& OutMissionInfo, bool& OutValidMission) const
{
    if (MissionDataTable && MissionName != NAME_None)
    {
        const FString ContextString;
        FFNAFMissionInfo* Row = MissionDataTable->FindRow<FFNAFMissionInfo>(MissionName, ContextString, false);
        if (Row)
        {
            OutMissionInfo.DisplayName = Row->DisplayName;
            OutMissionInfo.TaskNames = Row->TaskNames;
            OutMissionInfo.bValidForSurvival = Row->bValidForSurvival;
            OutMissionInfo.bShowAllTasks = Row->bShowAllTasks;
            OutMissionInfo.DependentMissions = Row->DependentMissions;
            OutValidMission = true;
            return;
        }
    }

    OutMissionInfo = FFNAFMissionInfo();
    OutValidMission = false;
}

void UFNAFMissionSystem::GetTaskInfo(const FName& TaskName, FFNAFMissionTaskInfo& OutMissionTaskInfo, bool& OutValidTask) const
{
    if (MissionTaskDataTable && TaskName != NAME_None)
    {
        const FString ContextString;
        FFNAFMissionTaskInfo* Row = MissionTaskDataTable->FindRow<FFNAFMissionTaskInfo>(TaskName, ContextString, false);
        if (Row)
        {
            OutMissionTaskInfo.TaskTitle = Row->TaskTitle;
            OutMissionTaskInfo.TaskDetails = Row->TaskDetails;
            OutMissionTaskInfo.LinkedMessage = Row->LinkedMessage;
            OutMissionTaskInfo.ItemsEarned = Row->ItemsEarned;
            OutMissionTaskInfo.ActivatedStates = Row->ActivatedStates;
            OutMissionTaskInfo.DeactivatedStates = Row->DeactivatedStates;
            OutMissionTaskInfo.LocationActor = Row->LocationActor;
            OutMissionTaskInfo.FreddySettings = Row->FreddySettings;
            OutMissionTaskInfo.ClockSettings = Row->ClockSettings;
            OutMissionTaskInfo.bSkipEnabled = Row->bSkipEnabled;
            OutValidTask = true;
            return;
        }
    }

    OutMissionTaskInfo = FFNAFMissionTaskInfo();
    OutValidTask = false;
}

void UFNAFMissionSystem::GetTaskByIndex(const FName& MissionName, int32 TaskIndex, FFNAFMissionTaskInfo& OutMissionTaskInfo, bool& OutValidTask) const
{
    if (TaskIndex < 0)
    {
        OutMissionTaskInfo = FFNAFMissionTaskInfo();
        OutValidTask = false;
        return;
    }

    FFNAFMissionInfo MissionInfo;
    bool bMissionValid = false;
    GetMissionInfo(MissionName, MissionInfo, bMissionValid);

    if (!bMissionValid || TaskIndex >= MissionInfo.TaskNames.Num())
    {
        OutMissionTaskInfo = FFNAFMissionTaskInfo();
        OutValidTask = false;
        return;
    }

    GetTaskInfo(MissionInfo.TaskNames[TaskIndex], OutMissionTaskInfo, OutValidTask);
}

void UFNAFMissionSystem::GetMissionState(const FName& MissionName, FFNAFMissionState& OutMissionState, bool& OutValidMission) const
{
    // In survival mode, return a synthetic "valid" state with Status=None to hide non-survival missions
    if (IsInSurvivalMode())
    {
        FFNAFMissionInfo MissionInfo;
        bool bInfoValid = false;
        GetMissionInfo(MissionName, MissionInfo, bInfoValid);

        if (!bInfoValid || !MissionInfo.bValidForSurvival)
        {
            OutValidMission = true;
            OutMissionState.Name = MissionName;
            OutMissionState.Status = EMissionStatus::None;
            OutMissionState.InfoState = 0;
            return;
        }
    }

    const FFNAFMissionState* Found = FindMissionStateEntry(MissionName);
    if (Found)
    {
        OutMissionState = *Found;
        OutValidMission = true;
    }
    else
    {
        OutMissionState.Name = FName(TEXT("Invalid"));
        OutMissionState.Status = EMissionStatus::None;
        OutMissionState.InfoState = 0;
        OutMissionState.CompletedTasks.Empty();
        OutValidMission = false;
    }
}

TArray<FName> UFNAFMissionSystem::GetAllMissionNames() const
{
    TArray<FName> Result;
    if (MissionDataTable)
    {
        Result = MissionDataTable->GetRowNames();
    }
    return Result;
}

TArray<FFNAFMissionState> UFNAFMissionSystem::GetTrackedMissions() const
{
    return MissionStates.Array();
}

bool UFNAFMissionSystem::IsValidMission(const FName& MissionName) const
{
    if (!MissionDataTable)
    {
        return false;
    }

    TArray<FName> RowNames = MissionDataTable->GetRowNames();
    return RowNames.Contains(MissionName);
}

bool UFNAFMissionSystem::IsActiveMission(const FName& MissionName) const
{
    const FFNAFMissionState* Found = FindMissionStateEntry(MissionName);
    return Found && Found->Status == EMissionStatus::Active;
}

bool UFNAFMissionSystem::IsCompletedMission(const FName& MissionName) const
{
    // Survival mode: treat non-survival missions as completed so they don't block progression
    if (IsInSurvivalMode())
    {
        FFNAFMissionInfo MissionInfo;
        bool bInfoValid = false;
        GetMissionInfo(MissionName, MissionInfo, bInfoValid);

        if (!bInfoValid || !MissionInfo.bValidForSurvival)
        {
            return true;
        }
    }

    const FFNAFMissionState* Found = FindMissionStateEntry(MissionName);
    return Found && Found->Status == EMissionStatus::Complete;
}

bool UFNAFMissionSystem::IsMissionActiveOrCompleted(const FName& MissionName) const
{
    if (IsInSurvivalMode())
    {
        FFNAFMissionInfo MissionInfo;
        bool bInfoValid = false;
        GetMissionInfo(MissionName, MissionInfo, bInfoValid);

        if (!bInfoValid || !MissionInfo.bValidForSurvival)
        {
            return true;
        }
    }

    const FFNAFMissionState* Found = FindMissionStateEntry(MissionName);
    if (Found && Found->Status == EMissionStatus::Active)
    {
        return true;
    }

    return IsCompletedMission(MissionName);
}

void UFNAFMissionSystem::AddActiveMission(const FName& MissionName)
{
    FFNAFMissionInfo MissionInfo;
    bool bMissionValid = false;
    GetMissionInfo(MissionName, MissionInfo, bMissionValid);

    if (!bMissionValid)
    {
        return;
    }

    if (ShouldFilterForSurvival(MissionInfo))
    {
        return;
    }

    FFNAFMissionState* Existing = FindMissionStateEntry(MissionName);

    if (Existing)
    {
        if (Existing->Status == EMissionStatus::Active || Existing->Status == EMissionStatus::Complete)
        {
            return;
        }

        Existing->Status = EMissionStatus::Active;
    }
    else
    {
        FFNAFMissionState NewState;
        NewState.Name = MissionName;
        NewState.Status = EMissionStatus::Active;
        NewState.InfoState = 0;
        MissionStates.Add(NewState);
    }

    if (OnNewActiveMissionAdded.IsBound())
    {
        const FFNAFMissionState* Entry = FindMissionStateEntry(MissionName);
        if (Entry)
        {
            OnNewActiveMissionAdded.Broadcast(MissionName, *Entry, MissionInfo);
        }
    }
}

void UFNAFMissionSystem::RemoveActiveMission(const FName& MissionName)
{
    FFNAFMissionInfo MissionInfo;
    bool bMissionValid = false;
    GetMissionInfo(MissionName, MissionInfo, bMissionValid);

    if (!bMissionValid)
    {
        return;
    }

    if (ShouldFilterForSurvival(MissionInfo))
    {
        return;
    }

    FFNAFMissionState* Found = FindMissionStateEntry(MissionName);
    if (Found && Found->Status == EMissionStatus::Active)
    {
        Found->Status = EMissionStatus::None;

        if (OnActiveMissionRemoved.IsBound())
        {
            OnActiveMissionRemoved.Broadcast(MissionName, *Found, MissionInfo);
        }
    }
}

void UFNAFMissionSystem::CompleteMission(const FName& MissionName)
{
    FFNAFMissionInfo MissionInfo;
    bool bMissionValid = false;
    GetMissionInfo(MissionName, MissionInfo, bMissionValid);

    if (!bMissionValid)
    {
        return;
    }

    FFNAFMissionState* Existing = FindMissionStateEntry(MissionName);

    if (Existing)
    {
        Existing->Status = EMissionStatus::Complete;
    }
    else
    {
        FFNAFMissionState NewState;
        NewState.Name = MissionName;
        NewState.Status = EMissionStatus::Complete;
        NewState.InfoState = 0;
        MissionStates.Add(NewState);
    }

    if (OnMissionCompleted.IsBound())
    {
        const FFNAFMissionState* Entry = FindMissionStateEntry(MissionName);
        if (Entry)
        {
            OnMissionCompleted.Broadcast(MissionName, *Entry, MissionInfo);
        }
    }
}

/*
 * IDA: operates on a local copy from GetMissionState, but semantically
 * the task completion must persist in the TSet. We modify the TSet entry
 * directly for correct behavior.
 */
void UFNAFMissionSystem::CompleteMissionTask(const FName& MissionName, int32 TaskIndex)
{
    FFNAFMissionState* Entry = FindMissionStateEntry(MissionName);
    if (!Entry)
    {
        return;
    }

    int32 FoundIndex = Entry->CompletedTasks.Find(TaskIndex);
    if (FoundIndex == INDEX_NONE)
    {
        FoundIndex = Entry->CompletedTasks.Add(TaskIndex);
    }

    if (OnActiveMissionUpdated.IsBound() && FoundIndex >= 0 && FoundIndex < Entry->CompletedTasks.Num())
    {
        NotifyMissionUpdate(MissionName);
    }
}

void UFNAFMissionSystem::SetMissionInfoState(const FName& MissionName, int32 InfoState)
{
    FFNAFMissionInfo MissionInfo;
    bool bMissionValid = false;
    GetMissionInfo(MissionName, MissionInfo, bMissionValid);

    if (!bMissionValid)
    {
        return;
    }

    if (ShouldFilterForSurvival(MissionInfo))
    {
        return;
    }

    FFNAFMissionState* Existing = FindMissionStateEntry(MissionName);

    if (Existing)
    {
        if (Existing->InfoState != InfoState)
        {
            Existing->InfoState = InfoState;

            if (OnActiveMissionUpdated.IsBound())
            {
                OnActiveMissionUpdated.Broadcast(MissionName, *Existing, MissionInfo);
            }
        }
    }
    else
    {
        FFNAFMissionState NewState;
        NewState.Name = MissionName;
        NewState.Status = EMissionStatus::Active;
        NewState.InfoState = InfoState;
        MissionStates.Add(NewState);

        if (OnNewActiveMissionAdded.IsBound())
        {
            const FFNAFMissionState* Entry = FindMissionStateEntry(MissionName);
            if (Entry)
            {
                OnNewActiveMissionAdded.Broadcast(MissionName, *Entry, MissionInfo);
            }
        }
    }
}

void UFNAFMissionSystem::BranchMissionStatus(const FName& MissionName, EMissionStatus& Status)
{
    if (IsInSurvivalMode())
    {
        FFNAFMissionInfo MissionInfo;
        bool bInfoValid = false;
        GetMissionInfo(MissionName, MissionInfo, bInfoValid);

        if (!bInfoValid || !MissionInfo.bValidForSurvival)
        {
            Status = EMissionStatus::None;
            return;
        }
    }

    const FFNAFMissionState* Found = FindMissionStateEntry(MissionName);
    Status = Found ? Found->Status : EMissionStatus::None;
}

void UFNAFMissionSystem::NotifyMissionUpdate(const FName& MissionName)
{
    if (OnActiveMissionUpdated.IsBound())
    {
        FFNAFMissionInfo MissionInfo;
        bool bInfoValid = false;
        GetMissionInfo(MissionName, MissionInfo, bInfoValid);

        if (bInfoValid)
        {
            FFNAFMissionState MissionState;
            bool bStateValid = false;
            GetMissionState(MissionName, MissionState, bStateValid);

            if (bStateValid)
            {
                OnActiveMissionUpdated.Broadcast(MissionName, MissionState, MissionInfo);
            }
        }
    }
}

TArray<FFNAFMissionState> UFNAFMissionSystem::GetActiveMissions() const
{
    TArray<FFNAFMissionState> Result;

    for (auto It = MissionStates.CreateConstIterator(); It; ++It)
    {
        const FFNAFMissionState& State = *It;

        FFNAFMissionInfo MissionInfo;
        bool bInfoValid = false;
        GetMissionInfo(State.Name, MissionInfo, bInfoValid);

        if (!bInfoValid)
        {
            continue;
        }

        if (ShouldFilterForSurvival(MissionInfo))
        {
            continue;
        }

        if (State.Status == EMissionStatus::Active)
        {
            Result.Add(State);
        }
    }

    return Result;
}

TArray<FFNAFMissionState> UFNAFMissionSystem::GetCompletedMissions() const
{
    TArray<FFNAFMissionState> Result;

    for (auto It = MissionStates.CreateConstIterator(); It; ++It)
    {
        const FFNAFMissionState& State = *It;
        if (State.Status == EMissionStatus::Complete)
        {
            Result.Add(State);
        }
    }

    return Result;
}

/* Returns markers matching a mission name and state index. Negative marker index is a wildcard. */
TArray<AMissionMarker*> UFNAFMissionSystem::GetMarkersForMission(const FName& MissionName, int32 MissionStateIndex) const
{
    TArray<AMissionMarker*> Result;

    for (AMissionMarker* Marker : MissionMarkers)
    {
        if (Marker && Marker->GetMissionName() == MissionName)
        {
            int32 MarkerIndex = Marker->GetStateIndex();
            if (MarkerIndex == MissionStateIndex || MarkerIndex < 0)
            {
                Result.Add(Marker);
            }
        }
    }

    return Result;
}

/* Returns markers for all active missions. Negative marker index is a wildcard matching any InfoState. */
TArray<AMissionMarker*> UFNAFMissionSystem::GetAllCurrentMarkers() const
{
    TArray<AMissionMarker*> Result;

    for (AMissionMarker* Marker : MissionMarkers)
    {
        if (!Marker)
        {
            continue;
        }

        FName MarkerMissionName = Marker->GetMissionName();
        const FFNAFMissionState* State = FindMissionStateEntry(MarkerMissionName);

        if (State && State->Status == EMissionStatus::Active)
        {
            int32 MarkerIndex = Marker->GetStateIndex();
            if (MarkerIndex < 0 || MarkerIndex == State->InfoState)
            {
                Result.Add(Marker);
            }
        }
    }

    return Result;
}

TArray<AMissionMarker*> UFNAFMissionSystem::GetAllMissionMarkers() const
{
    return MissionMarkers;
}

UDataTable* UFNAFMissionSystem::GetMissionTable() const
{
    return MissionDataTable;
}

UDataTable* UFNAFMissionSystem::GetTaskTable() const
{
    return MissionTaskDataTable;
}

/*
 * Reverse-lookup: finds which mission contains a task whose LinkedMessage matches
 * MessageName, returning the mission name and the task's index within TaskNames.
 */
void UFNAFMissionSystem::GetMissionFromMessage(const FName& MessageName, FName& OutMissionName, int32& OutMissionStateIndex) const
{
    if (!MissionTaskDataTable || !MissionDataTable)
    {
        OutMissionName = NAME_None;
        OutMissionStateIndex = -1;
        return;
    }

    FName FoundTaskName = NAME_None;
    const TMap<FName, uint8*>& TaskRowMap = MissionTaskDataTable->GetRowMap();
    for (const auto& Pair : TaskRowMap)
    {
        const uint8* RowData = Pair.Value;
        if (RowData)
        {
            const FFNAFMissionTaskInfo* TaskInfo = reinterpret_cast<const FFNAFMissionTaskInfo*>(RowData);
            if (TaskInfo->LinkedMessage == MessageName)
            {
                FoundTaskName = Pair.Key;
                break;
            }
        }
    }

    if (FoundTaskName == NAME_None)
    {
        OutMissionName = NAME_None;
        OutMissionStateIndex = -1;
        return;
    }

    const TMap<FName, uint8*>& MissionRowMap = MissionDataTable->GetRowMap();
    for (const auto& Pair : MissionRowMap)
    {
        const uint8* RowData = Pair.Value;
        if (RowData)
        {
            const FFNAFMissionInfo* MissionInfo = reinterpret_cast<const FFNAFMissionInfo*>(RowData);
            for (int32 i = 0; i < MissionInfo->TaskNames.Num(); ++i)
            {
                if (MissionInfo->TaskNames[i] == FoundTaskName)
                {
                    OutMissionName = Pair.Key;
                    OutMissionStateIndex = i;
                    return;
                }
            }
        }
    }

    OutMissionName = NAME_None;
    OutMissionStateIndex = -1;
}

void UFNAFMissionSystem::GetMissionFromTask(const FName& TaskName, FFNAFMissionInfo& OutMissionInfo, bool& OutValidMission) const
{
    if (!MissionDataTable)
    {
        OutMissionInfo = FFNAFMissionInfo();
        OutValidMission = false;
        return;
    }

    const TMap<FName, uint8*>& RowMap = MissionDataTable->GetRowMap();
    for (const auto& Pair : RowMap)
    {
        const uint8* RowData = Pair.Value;
        if (RowData)
        {
            const FFNAFMissionInfo* MissionInfo = reinterpret_cast<const FFNAFMissionInfo*>(RowData);
            for (const FName& TaskEntry : MissionInfo->TaskNames)
            {
                if (TaskEntry == TaskName)
                {
                    OutMissionInfo.DisplayName = MissionInfo->DisplayName;
                    OutMissionInfo.TaskNames = MissionInfo->TaskNames;
                    OutMissionInfo.bValidForSurvival = MissionInfo->bValidForSurvival;
                    OutMissionInfo.bShowAllTasks = MissionInfo->bShowAllTasks;
                    OutMissionInfo.DependentMissions = MissionInfo->DependentMissions;
                    OutValidMission = true;
                    return;
                }
            }
        }
    }

    OutMissionInfo = FFNAFMissionInfo();
    OutValidMission = false;
}

// TODO: Restore from IDA when pseudocode is available
void UFNAFMissionSystem::GetAreaMarkerCounts(TMap<ELevelArea, int32>& MapOfCounts) const
{
}

void UFNAFMissionSystem::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    TArray<FFNAFMissionState> StateArray = MissionStates.Array();
    SaveDataObject->MissionState = MoveTemp(StateArray);
}

void UFNAFMissionSystem::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    MissionStates.Empty();

    for (const FFNAFMissionState& SavedState : SaveDataObject->MissionState)
    {
        MissionStates.Add(SavedState);
    }
}
