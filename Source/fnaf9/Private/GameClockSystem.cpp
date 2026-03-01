#include "GameClockSystem.h"
#include "FNAFSaveData.h"
#include "GameClockSettings.h"
#include "WorldStateSystem.h"
#include "EFNAFGameState.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameClock, Log, All);

// Handles negative time edge case (hour = -1, minute wrapped)
static void ConvertSecondsToHourMinute(float Seconds, int32& OutHour, int32& OutMinute)
{
    if (Seconds >= 0.0f)
    {
        int32 TotalMinutes = (int32)Seconds / 60;
        OutHour = TotalMinutes / 60;
        OutMinute = TotalMinutes % 60;
    }
    else
    {
        OutHour = -1;
        OutMinute = (int32)(Seconds * 0.016666668f + 60.000004f);
    }
}

UGameClockSystem::UGameClockSystem()
    : TimerHandle()
    , bIsClockRunning(false)
    , CurrentGameTime(0.0f)
    , bEndGameReached(false)
    , TotalGameTimeSeconds(0.0f)
    , TotalRealTimeSeconds(0.0f)
    , SecondsPerTick(0.0f)
    , ELEVEN_PM(-1)
{
}

/*
 * SetupSystem -- Resets clock, reads timing from GameClockSettings CDO,
 * and in Normal mode registers Moonman delegates at :50/:55/:00 for hours 0-6.
 */
void UGameClockSystem::SetupSystem(EFNAFGameType GameType)
{
    CurrentGameTime = 0.0f;
    bIsClockRunning = false;

    // Clear existing timer
    UWorld* World = GetWorld();
    if (World)
    {
        FTimerManager& TimerManager = World->GetTimerManager();
        if (TimerManager.TimerExists(TimerHandle))
        {
            TimerManager.ClearTimer(TimerHandle);
        }
    }
    TimerHandle.Invalidate();

    // Clear time delegates
    TimeDelegates.Empty();

    // Read settings from CDO
    const UGameClockSettings* Settings = GetDefault<UGameClockSettings>();

    TotalGameTimeSeconds = Settings->TotalGameTimeHours * 3600.0f;

    if (GameType == EFNAFGameType::Normal)
    {
        TotalRealTimeSeconds = Settings->TotalRealTimeHours * 3600.0f;
        SecondsPerTick = (TotalGameTimeSeconds / TotalRealTimeSeconds) * Settings->TickRate;

        // Bind Moonman delegates to this object's trigger functions
        MoonmanLiteDelegate.BindUFunction(this, FName("OnMoonmanLiteTriggered"));
        MoonmanIntermediateDelegate.BindUFunction(this, FName("OnMoonmanIntermediateTriggered"));
        MoonmanDangerDelegate.BindUFunction(this, FName("OnMoonmanDangerTriggered"));

        // IDA: Loop v3 = 0..6, registers time events for each hour cycle
        //   :50 → MoonmanLite
        //   :55 → MoonmanIntermediate
        //   :00 (next hour) → MoonmanDanger
        int32 Hour = 0;
        do
        {
            SetGameTimeEvent(MoonmanLiteDelegate, Hour, 50);
            SetGameTimeEvent(MoonmanIntermediateDelegate, Hour, 55);
            Hour++;
            SetGameTimeEvent(MoonmanDangerDelegate, Hour, 0);
        } while (Hour < 7);
    }
    else if (GameType == EFNAFGameType::ChowdaMode)
    {
        // IDA: ChowdaMode uses same formula but different CDO fields (same offsets though)
        TotalRealTimeSeconds = Settings->TotalRealTimeHours * 3600.0f;
        SecondsPerTick = (TotalGameTimeSeconds / TotalRealTimeSeconds) * Settings->TickRate;
    }
}

void UGameClockSystem::SetClockRunning(bool bRunClock)
{
    if (bEndGameReached)
    {
        return;
    }

    // Broadcast state change if different
    if (bIsClockRunning != bRunClock)
    {
        OnGameClockStateChange.Broadcast(bRunClock);
    }

    bIsClockRunning = bRunClock;

    if (bRunClock)
    {
        // Read tick rate from settings CDO
        const UGameClockSettings* Settings = GetDefault<UGameClockSettings>();
        float TickRate = Settings->TickRate;

        // Set up looping timer
        UWorld* World = GetWorld();
        if (World)
        {
            World->GetTimerManager().SetTimer(
                TimerHandle,
                this,
                &UGameClockSystem::GameClockTick,
                TickRate,
                true    // bLoop
            );
        }
    }
    else
    {
        // Clear timer
        UWorld* World = GetWorld();
        if (World)
        {
            FTimerManager& TimerManager = World->GetTimerManager();
            if (TimerManager.TimerExists(TimerHandle))
            {
                TimerManager.ClearTimer(TimerHandle);
            }
        }
        TimerHandle.Invalidate();
    }
}

// Advances game time, broadcasts tick delegates, fires time events, and handles end-of-game
void UGameClockSystem::GameClockTick()
{
    CurrentGameTime += SecondsPerTick;

    int32 Hour, Minute;
    ConvertSecondsToHourMinute(CurrentGameTime, Hour, Minute);

    // Broadcast tick delegates
    OnGameClockTick.Broadcast(Hour, Minute);
    OnGameClockTickDelta.Broadcast(SecondsPerTick);
    OnGameClockTimeChanged.Broadcast(Hour, Minute);

    // Check and fire time delegates
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (!TD.bFired)
        {
            // IDA: checks delegate is valid (FName != NAME_None)
            if (TD.Delegate.IsBound())
            {
                // Fire if current time >= delegate time
                if (Hour > TD.Hour || (Hour == TD.Hour && Minute >= TD.Minute))
                {
                    TD.Delegate.ExecuteIfBound();
                    TD.bFired = true;
                }
            }
        }
    }

    // Check if game time has ended
    if (CurrentGameTime >= TotalGameTimeSeconds)
    {
        // Fire all game end delegates
        for (int32 i = 0; i < GameEndDelegates.Num(); i++)
        {
            if (GameEndDelegates[i].IsBound())
            {
                GameEndDelegates[i].ExecuteIfBound();
            }
        }

        // Clamp to total and stop
        CurrentGameTime = TotalGameTimeSeconds;
        SetClockRunning(false);
        bEndGameReached = true;
    }
}

void UGameClockSystem::SetClockRate(int32 TotalRealHours)
{
    const UGameClockSettings* Settings = GetDefault<UGameClockSettings>();

    TotalRealTimeSeconds = (float)TotalRealHours * 3600.0f;
    SecondsPerTick = (Settings->TotalGameTimeHours / (float)TotalRealHours) * Settings->TickRate;
}

void UGameClockSystem::SetClockRateInMinutesPerHour(int32 MinutesPerHour)
{
    const UGameClockSettings* Settings = GetDefault<UGameClockSettings>();

    TotalRealTimeSeconds = (float)MinutesPerHour * Settings->TotalGameTimeHours * 60.0f;
    SecondsPerTick = (60.0f / (float)MinutesPerHour) * Settings->TickRate;
}

void UGameClockSystem::SetCurrentTime(int32 Hour, int32 Minute, bool bPlayDelegates)
{
    float NewTime = (float)(Minute * 60) + (float)Hour * 3600.0f;
    CurrentGameTime = NewTime;

    if (NewTime < TotalGameTimeSeconds)
    {
        bEndGameReached = false;
    }

    // Update time delegates
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (TD.bFired)
        {
            // Un-fire if new time is before this delegate
            if (Hour < TD.Hour || (Hour == TD.Hour && Minute < TD.Minute))
            {
                TD.bFired = false;
            }
        }
        else
        {
            // Fire if new time is at or past this delegate
            if (Hour > TD.Hour || (Hour == TD.Hour && Minute >= TD.Minute))
            {
                if (bPlayDelegates)
                {
                    if (TD.Delegate.IsBound())
                    {
                        TD.Delegate.ExecuteIfBound();
                    }
                }
                TD.bFired = true;
            }
        }
    }

    OnGameClockTimeChanged.Broadcast(Hour, Minute);
}

void UGameClockSystem::SetCurrentHour(int32 Hour)
{
    // Extract current minute
    int32 CurrentMinute;
    if (CurrentGameTime >= 0.0f)
    {
        CurrentMinute = ((int32)CurrentGameTime / 60) % 60;
    }
    else
    {
        CurrentMinute = (int32)(CurrentGameTime * 0.016666668f + 60.000004f);
    }

    float NewTime = (float)(CurrentMinute * 60) + (float)Hour * 3600.0f;
    CurrentGameTime = NewTime;

    if (NewTime < TotalGameTimeSeconds)
    {
        bEndGameReached = false;
    }

    // Re-evaluate time delegates
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (TD.bFired)
        {
            if (Hour < TD.Hour || (Hour == TD.Hour && CurrentMinute < TD.Minute))
            {
                TD.bFired = false;
            }
        }
        else
        {
            if (Hour > TD.Hour || (Hour == TD.Hour && CurrentMinute >= TD.Minute))
            {
                TD.bFired = true;
            }
        }
    }

    OnGameClockTimeChanged.Broadcast(Hour, CurrentMinute);
}

void UGameClockSystem::SetCurrentMinute(int32 Minute)
{
    // Extract current hour
    int32 CurrentHour;
    if (CurrentGameTime >= 0.0f)
    {
        CurrentHour = (int32)CurrentGameTime / 3600;
    }
    else
    {
        CurrentHour = -1;
    }

    float NewTime = (float)(Minute * 60) + (float)CurrentHour * 3600.0f;
    CurrentGameTime = NewTime;

    if (NewTime < TotalGameTimeSeconds)
    {
        bEndGameReached = false;
    }

    // Re-evaluate time delegates
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (TD.bFired)
        {
            if (CurrentHour < TD.Hour || (CurrentHour == TD.Hour && Minute < TD.Minute))
            {
                TD.bFired = false;
            }
        }
        else
        {
            if (CurrentHour > TD.Hour || (CurrentHour == TD.Hour && Minute >= TD.Minute))
            {
                TD.bFired = true;
            }
        }
    }

    OnGameClockTimeChanged.Broadcast(CurrentHour, Minute);
}

// Only adds the delegate if the specified time is in the future
void UGameClockSystem::SetGameTimeEvent(FOnTimeReachedDynamicDelegate Delegate, int32 Hour, int32 Minute)
{
    int32 CurrentHour, CurrentMinute;
    ConvertSecondsToHourMinute(CurrentGameTime, CurrentHour, CurrentMinute);

    // Only add if target time is strictly in the future
    if (CurrentHour < Hour || (CurrentHour == Hour && CurrentMinute < Minute))
    {
        TimeDelegates.Add(FGameClockTimeDelegate(Hour, Minute, Delegate));
    }
}

void UGameClockSystem::SetGameEndEvent(FOnTimeReachedDynamicDelegate Delegate)
{
    GameEndDelegates.Add(Delegate);
}

// Advances clock to start of next hour, re-evaluates delegates, resets world state to Normal
void UGameClockSystem::StartNextHour()
{
    int32 CurrentHour;
    if (CurrentGameTime >= 0.0f)
    {
        CurrentHour = (int32)CurrentGameTime / 3600;
    }
    else
    {
        CurrentHour = -1;
    }

    // Mark all delegates at or before (CurrentHour+1, :00) as fired
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        int32 TDHour = TD.Hour;
        // IDA: if (TDHour <= CurrentHour || (TDHour == CurrentHour+1 && TD.Minute == 0))
        if (TDHour <= CurrentHour || (TDHour == CurrentHour + 1 && TD.Minute == 0))
        {
            TD.bFired = true;
        }
    }

    int32 NextHour = CurrentHour + 1;
    float NewTime = (float)NextHour * 3600.0f;
    CurrentGameTime = NewTime;

    if (NewTime < TotalGameTimeSeconds)
    {
        bEndGameReached = false;
    }

    // Re-evaluate delegates at new time (NextHour, 0)
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (TD.bFired)
        {
            // Un-fire if new time is before this delegate
            if (NextHour < TD.Hour || (NextHour == TD.Hour && 0 < TD.Minute))
            {
                TD.bFired = false;
            }
        }
        else
        {
            // Mark fired if new time is at or past
            if (NextHour > TD.Hour || (NextHour == TD.Hour && TD.Minute <= 0))
            {
                TD.bFired = true;
            }
        }
    }

    OnGameClockTimeChanged.Broadcast(NextHour, 0);

    // IDA: Reset WorldStateSystem to Normal
    UWorldStateSystem* WorldStateSys = GetGameInstance() ?
        GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        WorldStateSys->SetWorldState(EFNAFGameState::Normal);
    }
}

void UGameClockSystem::ResetTimeDelegatesUpTo(int32 Hour, int32 Minute)
{
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (TD.bFired)
        {
            if (Hour < TD.Hour || (Hour == TD.Hour && Minute < TD.Minute))
            {
                TD.bFired = false;
            }
        }
        else
        {
            if (Hour > TD.Hour || (Hour == TD.Hour && Minute >= TD.Minute))
            {
                TD.bFired = true;
            }
        }
    }
}

// Moonman state triggers -- each sets the corresponding world state
void UGameClockSystem::OnNormalModeTriggered()
{
    UWorldStateSystem* WorldStateSys = GetGameInstance() ?
        GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        WorldStateSys->SetWorldState(EFNAFGameState::Normal);
    }
}

void UGameClockSystem::OnMoonmanLiteTriggered()
{
    UWorldStateSystem* WorldStateSys = GetGameInstance() ?
        GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        WorldStateSys->SetWorldState(EFNAFGameState::MoonManLite);
    }
}

void UGameClockSystem::OnMoonmanIntermediateTriggered()
{
    UWorldStateSystem* WorldStateSys = GetGameInstance() ?
        GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        WorldStateSys->SetWorldState(EFNAFGameState::MoonManIntermediate);
    }
}

void UGameClockSystem::OnMoonmanDangerTriggered()
{
    UWorldStateSystem* WorldStateSys = GetGameInstance() ?
        GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        WorldStateSys->SetWorldState(EFNAFGameState::MoonManDanger);
    }
}

bool UGameClockSystem::IsClockRunning() const
{
    return bIsClockRunning;
}

float UGameClockSystem::GetCurrentTimeInSeconds() const
{
    return CurrentGameTime;
}

void UGameClockSystem::GetCurrentTime(int32& Hour, int32& Minute) const
{
    ConvertSecondsToHourMinute(CurrentGameTime, Hour, Minute);
}

// Save/Load
void UGameClockSystem::OnCheckpointSave_Implementation(UFNAFSaveData* SaveDataObject)
{
    int32 Hour, Minute;
    ConvertSecondsToHourMinute(CurrentGameTime, Hour, Minute);
    SaveDataObject->Hour = Hour;
    SaveDataObject->Minute = Minute;
}

void UGameClockSystem::OnCheckpointLoad_Implementation(UFNAFSaveData* SaveDataObject)
{
    int32 Hour = SaveDataObject->Hour;
    int32 Minute = SaveDataObject->Minute;

    float NewTime = (float)(Minute * 60) + (float)Hour * 3600.0f;
    CurrentGameTime = NewTime;

    if (NewTime < TotalGameTimeSeconds)
    {
        bEndGameReached = false;
    }

    // Re-evaluate all time delegates
    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        FGameClockTimeDelegate& TD = TimeDelegates[i];
        if (TD.bFired)
        {
            // Un-fire if loaded time is before this delegate
            if (Hour < TD.Hour || (Hour == TD.Hour && Minute < TD.Minute))
            {
                TD.bFired = false;
            }
        }
        else
        {
            // Mark fired if loaded time is at or past this delegate
            if (Hour > TD.Hour || (Hour == TD.Hour && Minute >= TD.Minute))
            {
                TD.bFired = true;
            }
        }
    }

    OnGameClockTimeChanged.Broadcast(Hour, Minute);
}

void UGameClockSystem::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    OnCheckpointSave_Implementation(SaveDataObject);
}

void UGameClockSystem::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    OnCheckpointLoad_Implementation(SaveDataObject);
}

void UGameClockSystem::PostSaveGame_Implementation()
{
    // No-op in base implementation
}

void UGameClockSystem::PostGameLoad_Implementation()
{
    // No-op in base implementation
}

// Debug logging of all connected time and game-end delegates
void UGameClockSystem::LogConnectedDelegates()
{
    FString LogStr = TEXT("Time Delegates: \n");

    for (int32 i = 0; i < TimeDelegates.Num(); i++)
    {
        const FGameClockTimeDelegate& TD = TimeDelegates[i];
        FString FuncName = TD.Delegate.GetFunctionName().ToString();
        // IDA also logs the bound object's name
        LogStr += FString::Printf(TEXT("\t%02d:%02d: %s\n"), TD.Hour, TD.Minute, *FuncName);
    }

    LogStr += TEXT("\nEnd Game Delegates:\n");

    for (int32 i = 0; i < GameEndDelegates.Num(); i++)
    {
        FString FuncName = GameEndDelegates[i].GetFunctionName().ToString();
        LogStr += FString::Printf(TEXT("\t %s\n"), *FuncName);
    }

    UE_LOG(LogGameClock, Log, TEXT("%s"), *LogStr);
}