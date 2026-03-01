#include "FNAFEventSystem.h"
#include "FNAFSaveData.h"
#include "FNAFEventObject.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "EngineUtils.h"

UFNAFEventSystem::UFNAFEventSystem()
    : CurrentAudioComp(nullptr)
    , CurrentEventActor(nullptr)
    , bIsRunning(false)
    , bIsPaused(false)
    , MinIdleTimeEventSeconds(0.0f)
    , MaxIdleTimeEventSeconds(0.0f)
    , WeightEventActors(0.0f)
    , MinDistanceSounds(0.0f)
    , MaxDistanceSounds(0.0f)
{
}

void UFNAFEventSystem::StoreEventTriggered(FName EventTag)
{
    SystemData.EventsTriggered.Add(EventTag);
}

bool UFNAFEventSystem::HasEventBeenTriggered(FName EventTag) const
{
    return SystemData.EventsTriggered.Contains(EventTag);
}

void UFNAFEventSystem::SetEventTimeSeconds(float MinTimeBetweenEvents, float MaxTimeBetweenEvents)
{
    MinIdleTimeEventSeconds = MinTimeBetweenEvents;
    MaxIdleTimeEventSeconds = MaxTimeBetweenEvents;
}

void UFNAFEventSystem::SetEventActorWeight(float NewWeight)
{
    WeightEventActors = NewWeight;
}

void UFNAFEventSystem::SetCurrentAudioComponent(UAudioComponent* EventSoundCue)
{
    CurrentAudioComp = EventSoundCue;
}

// Sets a one-shot timer with random delay; immediately pauses if bIsPaused
void UFNAFEventSystem::StartEventTimer()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    float RandomDelay = MinIdleTimeEventSeconds +
        FMath::FRand() * (MaxIdleTimeEventSeconds - MinIdleTimeEventSeconds);

    FTimerDelegate TimerDelegate;
    TimerDelegate.BindUObject(this, &UFNAFEventSystem::OnEventTimer);
    World->GetTimerManager().SetTimer(NextEventTimer, TimerDelegate, RandomDelay, false);

    if (bIsPaused)
    {
        World->GetTimerManager().PauseTimer(NextEventTimer);
    }
}

void UFNAFEventSystem::StopEventTimer()
{
    bIsRunning = false;

    UWorld* World = GetWorld();
    if (World)
    {
        FTimerManager& TimerManager = World->GetTimerManager();
        if (TimerManager.TimerExists(NextEventTimer))
        {
            TimerManager.ClearTimer(NextEventTimer);
        }
    }

    NextEventTimer.Invalidate();
}

// Only pauses if NextEventTimer is active AND not already paused
void UFNAFEventSystem::PauseEventSystem()
{
    if (NextEventTimer.IsValid() && !bIsPaused)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            World->GetTimerManager().PauseTimer(NextEventTimer);
        }
        bIsPaused = true;
    }
}

void UFNAFEventSystem::UnpauseEventSystem()
{
    if (NextEventTimer.IsValid() && bIsPaused)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            World->GetTimerManager().UnPauseTimer(NextEventTimer);
        }
        bIsPaused = false;
    }
}

// Polls CurrentEventActor each second; clears and restarts cycle when finished
void UFNAFEventSystem::OnEventObjectTick()
{
    if (!IsValid(CurrentEventActor) ||
        IFNAFEventObject::Execute_IsEventFinished(CurrentEventActor))
    {
        CurrentEventActor = nullptr;

        UWorld* World = GetWorld();
        if (World)
        {
            FTimerManager& TimerManager = World->GetTimerManager();
            if (TimerManager.TimerExists(WaitForDoneTimer))
            {
                TimerManager.ClearTimer(WaitForDoneTimer);
            }
        }

        WaitForDoneTimer.Invalidate();

        if (bIsRunning)
        {
            StartEventTimer();
        }
    }
}

// Unbinds self from the audio component's delegate, then restarts event timer
void UFNAFEventSystem::OnAudioFinished()
{
    if (CurrentAudioComp)
    {
        FScriptDelegate Delegate;
        Delegate.BindUFunction(this, FName(TEXT("OnAudioFinished")));
        CurrentAudioComp->OnAudioFinished.Remove(Delegate);
    }

    if (bIsRunning)
    {
        StartEventTimer();
    }
}

/* Main event system logic. When the idle timer fires:
   1. Collects actors implementing IFNAFEventObject where CanPlayEvent() is true
   2. If no actors or random roll > WeightEventActors, plays ambient sound
   3. Otherwise picks a random event actor and triggers it
   4. Schedules the next event timer if still running */
void UFNAFEventSystem::OnEventTimer()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        if (bIsRunning)
        {
            StartEventTimer();
        }
        return;
    }

    // Collect valid event actors
    TArray<AActor*> ValidEventActors;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor->GetClass() &&
            Actor->GetClass()->ImplementsInterface(UFNAFEventObject::StaticClass()))
        {
            if (IFNAFEventObject::Execute_CanPlayEvent(Actor))
            {
                ValidEventActors.Add(Actor);
            }
        }
    }

    int32 NumValidActors = ValidEventActors.Num();

    // Decide: ambient sound vs event actor
    if (NumValidActors <= 0 || FMath::FRand() > WeightEventActors)
    {
        // Ambient sound path
        if (CurrentAudioComp)
        {
            FScriptDelegate Delegate;
            Delegate.BindUFunction(this, FName(TEXT("OnAudioFinished")));
            CurrentAudioComp->OnAudioFinished.AddUnique(Delegate);

            APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
            if (CameraManager)
            {
                FVector CameraLocation = CameraManager->GetCameraLocation();

                float Distance = MinDistanceSounds +
                    FMath::FRand() * (MaxDistanceSounds - MinDistanceSounds);

                // Random unit direction via rejection sampling on unit sphere
                FVector RandomDir;
                float LengthSq;
                do
                {
                    RandomDir.X = FMath::FRand() * 2.0f - 1.0f;
                    RandomDir.Y = FMath::FRand() * 2.0f - 1.0f;
                    RandomDir.Z = FMath::FRand() * 2.0f - 1.0f;
                    LengthSq = RandomDir.SizeSquared();
                } while (LengthSq > 1.0f || LengthSq < 0.0001f);

                RandomDir.Normalize();

                FVector SoundLocation = CameraLocation + RandomDir * Distance;
                CurrentAudioComp->SetWorldLocation(SoundLocation);
                CurrentAudioComp->Play();
            }
        }
    }
    else
    {
        // Event actor path
        int32 PickedIndex = FMath::Clamp(
            FMath::FloorToInt(FMath::FRand() * (float)NumValidActors),
            0,
            NumValidActors - 1);

        CurrentEventActor = ValidEventActors[PickedIndex];

        IFNAFEventObject::Execute_TriggerEvent(CurrentEventActor);

        // Poll IsEventFinished every second
        FTimerDelegate TickDelegate;
        TickDelegate.BindUObject(this, &UFNAFEventSystem::OnEventObjectTick);
        World->GetTimerManager().SetTimer(WaitForDoneTimer, TickDelegate, 1.0f, true);
    }

    if (bIsRunning)
    {
        StartEventTimer();
    }
}

// ISaveHandlerInterface
void UFNAFEventSystem::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    SaveDataObject->EventSystemData = SystemData;
}

void UFNAFEventSystem::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    SystemData = SaveDataObject->EventSystemData;
}
