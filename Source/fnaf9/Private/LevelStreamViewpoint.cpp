#include "LevelStreamViewpoint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "FNAFLevelManager.h"
#include "StreamingLevelUtil.h"

DEFINE_LOG_CATEGORY_STATIC(LogLevelStream, Log, All);

ULevelStreamViewpoint::ULevelStreamViewpoint()
{
    PrimaryComponentTick.bCanEverTick = false;
    bStreamingEnable = false;
    bEnableStreamOnActivePawn = false;
    bLevelsLoadedSent = false;
}

void ULevelStreamViewpoint::BeginPlay()
{
    Super::BeginPlay();

    bLevelsLoadedSent = false;

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UFNAFLevelManager* LevelManager = World->GetSubsystem<UFNAFLevelManager>();
    if (!LevelManager)
    {
        return;
    }

    if (bEnableStreamOnActivePawn)
    {
        // Register as a pawn-based streaming source and always bind to the update delegate
        LevelManager->RegisterPawnStreamingSource(this);
        LevelManager->OnlevelsUpdated.AddDynamic(this, &ULevelStreamViewpoint::OnLevelManagerUpdated);
    }
    else
    {
        // For non-pawn sources, SetStreamingEnable handles both registration and delegate binding
        SetStreamingEnable(bStreamingEnable);
    }
}

void ULevelStreamViewpoint::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UFNAFLevelManager* LevelManager = World->GetSubsystem<UFNAFLevelManager>();
    if (!LevelManager)
    {
        return;
    }

    if (bEnableStreamOnActivePawn)
    {
        LevelManager->UnregisterPawnStreamingSource(this);
    }
    else
    {
        LevelManager->UnregisterStreamingSource(this);
    }
}

void ULevelStreamViewpoint::SetStreamingEnable(bool bEnable)
{
    // Reset the "levels loaded" notification if the streaming state actually changes
    if (bStreamingEnable != bEnable)
    {
        bLevelsLoadedSent = false;
    }

    bStreamingEnable = bEnable;

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UFNAFLevelManager* LevelManager = World->GetSubsystem<UFNAFLevelManager>();
    if (!LevelManager)
    {
		UE_LOG(LogLevelStream, Warning, TEXT("SetStreamingEnable: FNAFLevelManager subsystem not found"));
        return;
    }

    if (bStreamingEnable)
    {
        // Register this component as a streaming source
        if (bEnableStreamOnActivePawn)
        {
            LevelManager->RegisterPawnStreamingSource(this);
        }
        else
        {
            LevelManager->RegisterStreamingSource(this);
        }

        // Bind to the level manager update delegate if we haven't sent the loaded notification yet
        if (!bLevelsLoadedSent)
        {
            LevelManager->OnlevelsUpdated.AddUniqueDynamic(this, &ULevelStreamViewpoint::OnLevelManagerUpdated);
        }
    }
    else
    {
        // Unregister this component as a streaming source
        if (bEnableStreamOnActivePawn)
        {
            LevelManager->UnregisterPawnStreamingSource(this);
        }
        else
        {
            LevelManager->UnregisterStreamingSource(this);
        }

        // Unbind from the level manager update delegate
        LevelManager->OnlevelsUpdated.RemoveDynamic(this, &ULevelStreamViewpoint::OnLevelManagerUpdated);
    }
}

void ULevelStreamViewpoint::OnLevelManagerUpdated()
{
    // Only process if we haven't already sent the notification and the delegate is bound
    if (bLevelsLoadedSent || !OnLevelsLoaded.IsBound())
    {
        return;
    }

    TArray<TWeakObjectPtr<ULevelStreaming>> Levels = GetStreamingLevels();

    // Check if ALL levels are visible
    bool bAllVisible = true;
    for (const TWeakObjectPtr<ULevelStreaming>& WeakLevel : Levels)
    {
        ULevelStreaming* Level = WeakLevel.Get();
        if (!Level || !Level->IsLevelVisible())
        {
            bAllVisible = false;
            break;
        }
    }

    if (bAllVisible)
    {
        OnLevelsLoaded.Broadcast();
        bLevelsLoadedSent = true;
    }
}

TArray<TWeakObjectPtr<ULevelStreaming>> ULevelStreamViewpoint::GetStreamingLevels() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
		UE_LOG(LogLevelStream, Warning, TEXT("GetStreamingLevels: World is null"));
        return TArray<TWeakObjectPtr<ULevelStreaming>>();
    }

    UFNAFLevelManager* LevelManager = World->GetSubsystem<UFNAFLevelManager>();
    if (!LevelManager || !IsValid(LevelManager))
    {
		UE_LOG(LogLevelStream, Warning, TEXT("GetStreamingLevels: FNAFLevelManager subsystem not found"));
        return TArray<TWeakObjectPtr<ULevelStreaming>>();
    }

    const FStreamingLevelArray& LevelArray = LevelManager->GetLevelsForComponent(this);
    return LevelArray.Levels;
}

TArray<FName> ULevelStreamViewpoint::GetStreamingLevelNames() const
{
    TArray<FName> Result;

    UWorld* World = GetWorld();
    if (!World)
    {
        return Result;
    }

    UFNAFLevelManager* LevelManager = World->GetSubsystem<UFNAFLevelManager>();
    if (!LevelManager || !IsValid(LevelManager))
    {
        return Result;
    }

    const FStreamingLevelArray& LevelArray = LevelManager->GetLevelsForComponent(this);
    TArray<TWeakObjectPtr<ULevelStreaming>> Levels = LevelArray.Levels;

    Result.Reserve(Levels.Num());
    for (const TWeakObjectPtr<ULevelStreaming>& WeakLevel : Levels)
    {
        if (ULevelStreaming* Level = WeakLevel.Get())
        {
            Result.Add(Level->GetFName());
        }
    }

    return Result;
}

bool ULevelStreamViewpoint::AnyLevelsLoaded() const
{
    TArray<TWeakObjectPtr<ULevelStreaming>> Levels = GetStreamingLevels();

    for (const TWeakObjectPtr<ULevelStreaming>& WeakLevel : Levels)
    {
        if (WeakLevel.IsValid())
        {
            ULevelStreaming* Level = WeakLevel.Get();
            if (Level && Level->IsLevelLoaded())
            {
                return true;
            }
        }
    }

    return false;
}

bool ULevelStreamViewpoint::AllLevelsLoaded() const
{
    TArray<TWeakObjectPtr<ULevelStreaming>> Levels = GetStreamingLevels();

    for (const TWeakObjectPtr<ULevelStreaming>& WeakLevel : Levels)
    {
        if (!WeakLevel.IsValid())
        {
            return false;
        }

        ULevelStreaming* Level = WeakLevel.Get();
        if (!Level || !Level->IsLevelLoaded())
        {
            return false;
        }
    }

    return true;
}