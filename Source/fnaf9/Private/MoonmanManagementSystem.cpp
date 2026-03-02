#include "MoonmanManagementSystem.h"
#include "MoonmanSpawnPoint.h"
#include "WorldStateSystem.h"
#include "GameClockSystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"

UMoonmanManagementSystem::UMoonmanManagementSystem()
{
}

void UMoonmanManagementSystem::RegisterSpawn(AMoonmanSpawnPoint* InSpawnPoint)
{
    MMSpawnPoints.Add(InSpawnPoint);
    OnMMRegisterSpawn.Broadcast(InSpawnPoint);
}

void UMoonmanManagementSystem::UnRegisterSpawn(AMoonmanSpawnPoint* InSpawnPoint)
{
    MMSpawnPoints.Remove(InSpawnPoint);
    OnMMUnregisterSpawn.Broadcast(InSpawnPoint);
}

TArray<AMoonmanSpawnPoint*> UMoonmanManagementSystem::GetAllMMSpawnPoints()
{
    return MMSpawnPoints;
}

// Filters spawn points by animation category (standing, crawling, etc.)
TArray<AMoonmanSpawnPoint*> UMoonmanManagementSystem::GetAllMMSpawnPointsFor(EMMAnimCategory MMAnimation) const
{
    TArray<AMoonmanSpawnPoint*> Result;

    for (AMoonmanSpawnPoint* SpawnPoint : MMSpawnPoints)
    {
        if (SpawnPoint && SpawnPoint->GetMMAnimCategory() == MMAnimation)
        {
            Result.Add(SpawnPoint);
        }
    }

    return Result;
}

// Uses TActorIterator to find all AMoonmanSpawnPoint actors already in the level
void UMoonmanManagementSystem::FindAllSpawnPoints()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    for (TActorIterator<AMoonmanSpawnPoint> It(World); It; ++It)
    {
        AMoonmanSpawnPoint* SpawnPoint = *It;
        if (SpawnPoint)
        {
            MMSpawnPoints.Add(SpawnPoint);
        }
    }
}

// When the world transitions to Normal state, start the moonman manager
void UMoonmanManagementSystem::OnWorldStateChanged(EFNAFGameState NewState, EFNAFGameState PrevState)
{
    if (NewState == EFNAFGameState::Normal)
    {
        StartManager();
    }
}

// Binds to WorldStateSystem's OnWorldStateChanged delegate and ensures GameClockSystem is initialized
void UMoonmanManagementSystem::StartManager()
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

    UWorldStateSystem* WorldStateSys = GameInstance->GetSubsystem<UWorldStateSystem>();
    if (WorldStateSys)
    {
        WorldStateSys->OnWorldStateChanged.AddDynamic(this, &UMoonmanManagementSystem::OnWorldStateChanged);
    }

    /* IDA touches UGameClockSystem without storing the result,
       suggesting this just ensures the clock system is available. */
    GameInstance->GetSubsystem<UGameClockSystem>();
}

void UMoonmanManagementSystem::PauseMoonmanManager()
{
    if (!TimerHandle.IsValid())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    World->GetTimerManager().PauseTimer(TimerHandle);
}

void UMoonmanManagementSystem::UnpauseMoonmanManager()
{
    if (!TimerHandle.IsValid())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    World->GetTimerManager().UnPauseTimer(TimerHandle);
}

/* IDA: 5-second repeating timer. Callback is COMDAT-folded to an empty retn
   (identical empty bodies merged by linker). The timer exists as a state flag
   so PauseMoonmanManager/UnpauseMoonmanManager work correctly. */
void UMoonmanManagementSystem::StartMoonmanDangerManager()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FTimerDelegate TimerDelegate;
    TimerDelegate.BindLambda([](){});
    World->GetTimerManager().SetTimer(TimerHandle, TimerDelegate, 5.0f, true);
}

/* "Lite" mode discovers spawn points from the level first via FindAllSpawnPoints,
   then sets up the same 5-second repeating timer as danger mode. */
void UMoonmanManagementSystem::StartMoonmanLiteManager()
{
    FindAllSpawnPoints();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FTimerDelegate TimerDelegate;
    TimerDelegate.BindLambda([](){});
    World->GetTimerManager().SetTimer(TimerHandle, TimerDelegate, 5.0f, true);
}
