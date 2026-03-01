#include "fnaf9GameModeBase.h"
#include "RoomSystem.h"
#include "FNAFEventSystem.h"
#include "AIManagementSystem.h"
#include "FNAFSightSystem.h"
#include "TimerManager.h"

Afnaf9GameModeBase::Afnaf9GameModeBase()
{
    CurrentState = EFNAFGameState::Normal;
    bCurrentAIDisplay = false;
}

// Tries CallFunctionByNameWithArguments on self first, falls through to SightSystem
bool Afnaf9GameModeBase::ProcessConsoleExec(const TCHAR* Cmd, FOutputDevice& Ar, UObject* Executor)
{
    if (CallFunctionByNameWithArguments(Cmd, Ar, Executor, false))
    {
        return true;
    }

    UWorld* World = GetWorld();
    if (World)
    {
        UFNAFSightSystem* SightSys = World->GetSubsystem<UFNAFSightSystem>();
        if (SightSys)
        {
            SightSys->ProcessConsoleExec(Cmd, Ar, Executor);
        }
    }

    return false;
}

void Afnaf9GameModeBase::FinishRepairGame()
{
    CurrentState = EFNAFGameState::Normal;
}

// Pauses EventSystem timer and AIManagementSystem, removes all AI characters
void Afnaf9GameModeBase::StartRepairGame()
{
    CurrentState = EFNAFGameState::RepairGame;

    UWorld* World = GetWorld();
    if (World)
    {
        UGameInstance* GI = World->GetGameInstance();
        if (GI)
        {
            UFNAFEventSystem* EventSys = GI->GetSubsystem<UFNAFEventSystem>();
            if (EventSys)
            {
                EventSys->PauseEventSystem();
            }
        }

        UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
        if (AIMgr)
        {
            FTimerManager& TimerManager = World->GetTimerManager();
            TimerManager.PauseTimer(AIMgr->TimerHandle);
            AIMgr->RemoveAllCharacters();
        }
    }
}

// Sets bCurrentAIDisplay, calls 3 BP events, broadcasts delegate
void Afnaf9GameModeBase::SetAIDisplay(bool enable)
{
    bCurrentAIDisplay = enable;
    OnRoomHeatDisplayChanged(enable);
    OnAIPawnsVis(bCurrentAIDisplay);
    OnPlayerFlashlightVis(bCurrentAIDisplay);

    OnSetAIDisplay.Broadcast(enable);
}

void Afnaf9GameModeBase::ToggleFullAIDisplay()
{
    bCurrentAIDisplay = !bCurrentAIDisplay;
    OnRoomHeatDisplayChanged(bCurrentAIDisplay);
    OnAIPawnsVis(bCurrentAIDisplay);
    OnPlayerFlashlightVis(bCurrentAIDisplay);

    OnSetAIDisplay.Broadcast(bCurrentAIDisplay);
}

bool Afnaf9GameModeBase::IsAIDisplayOn() const
{
    return bCurrentAIDisplay;
}

// Writes directly to RoomSystem's bPOIDetectionVisibility field
void Afnaf9GameModeBase::POIDetectionVisible(bool bVisible) const
{
    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys)
        {
            RoomSys->POIDetectionVisibility(bVisible);
        }
    }
}

void Afnaf9GameModeBase::POIVisible(bool bVisible) const
{
    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys)
        {
            RoomSys->POIVisibility(bVisible);
        }
    }
}

// Thunks that forward to the BlueprintImplementableEvent
void Afnaf9GameModeBase::AIPawnsVis(bool bEnable)
{
    OnAIPawnsVis(bEnable);
}

void Afnaf9GameModeBase::ForceSpawnVanny()
{
    OnForceSpawnVanny();
}

void Afnaf9GameModeBase::PlayerFlashlightVis(bool bEnable)
{
    OnPlayerFlashlightVis(bEnable);
}

void Afnaf9GameModeBase::RoomHeatDisplay(bool enable)
{
    OnRoomHeatDisplayChanged(enable);
}
