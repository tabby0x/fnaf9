#include "StreamingLoadComponent.h"
#include "Engine/LevelStreamingVolume.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogStreamingLoad, Log, All);

UStreamingLoadComponent::UStreamingLoadComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bTurnVolumesOnAfterLoad = false;
    CurrentLevelIndex = 0;
    bIsStreamingLevels = false;
}

bool UStreamingLoadComponent::IsStreamingLevels() const
{
    return bIsStreamingLevels;
}

// Sets or clears bDisabled on every ALevelStreamingVolume in the world.
void UStreamingLoadComponent::EnableAllStreamingVolumes(bool bEnable)
{

    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(this, ALevelStreamingVolume::StaticClass(), VolumeActors);

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (Volume)
        {
            Volume->bDisabled = !bEnable;
        }
    }
}

/*
 * StartAsyncLoadForLocation - Entry point for pre-loading streaming levels around
 * a world position. Disables all streaming volumes, collects level names from
 * volumes encompassing the target location, then loads them sequentially.
 */
void UStreamingLoadComponent::StartAsyncLoadForLocation(const FVector& WorldLocation)
{

    if (bIsStreamingLevels)
    {
        return;
    }

    CurrentLevelIndex = 0;
    LevelsToStream.Empty();

    // Gather all streaming volume actors
    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(this, ALevelStreamingVolume::StaticClass(), VolumeActors);

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (!Volume)
        {
            continue;
        }

        // Disable EVERY volume in the world
        Volume->bDisabled = true;

        // If this volume contains the target location, collect its level names
        if (Volume->EncompassesPoint(WorldLocation, 0.0f))
        {
            const TArray<FName>& LevelNames = Volume->StreamingLevelNames;
            if (LevelNames.Num() > 0)
            {
                LevelsToStream.Append(LevelNames);
            }
        }
    }

    // Start loading or immediately complete if nothing to load
    if (LevelsToStream.Num() > 0)
    {
        bIsStreamingLevels = true;
        LoadNextLevel();
    }
    else
    {
        bIsStreamingLevels = false;
        if (OnLevelStreamingFinished.IsBound())
        {
            OnLevelStreamingFinished.Broadcast();
        }
    }
}

/*
 * LoadNextLevel - Loads LevelsToStream[CurrentLevelIndex] via LoadStreamLevel,
 * then increments the index. UUID is set to post-increment value.
 * bMakeVisibleAfterLoad = true, bShouldBlockOnLoad = false.
 */
void UStreamingLoadComponent::LoadNextLevel()
{
    FName LevelName = LevelsToStream[CurrentLevelIndex];
    CurrentLevelIndex++;

    FLatentActionInfo LatentInfo;
    LatentInfo.CallbackTarget = this;
    LatentInfo.ExecutionFunction = FName("OnLevelLoaded");
    LatentInfo.UUID = CurrentLevelIndex;
    LatentInfo.Linkage = 0;

    UGameplayStatics::LoadStreamLevel(this, LevelName, true, false, LatentInfo);
}

/*
 * OnLevelLoaded - Latent callback when a level finishes loading. If more remain,
 * continues with LoadNextLevel. Otherwise broadcasts OnLevelStreamingFinished and
 * optionally re-enables all streaming volumes (the "ignition switch" for
 * UFNAFLevelManager::Tick volume checks).
 */
void UStreamingLoadComponent::OnLevelLoaded()
{
    if (CurrentLevelIndex >= LevelsToStream.Num())
    {
        // All levels loaded
        bIsStreamingLevels = false;

        if (OnLevelStreamingFinished.IsBound())
        {
            OnLevelStreamingFinished.Broadcast();
        }

        // Re-enable streaming volumes if configured to do so
        if (bTurnVolumesOnAfterLoad)
        {
            TArray<AActor*> VolumeActors;
            UGameplayStatics::GetAllActorsOfClass(this, ALevelStreamingVolume::StaticClass(), VolumeActors);

            for (AActor* Actor : VolumeActors)
            {
                ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
                if (Volume)
                {
                    Volume->bDisabled = false;
                }
            }
        }
    }
    else
    {
        // More levels to load
        LoadNextLevel();
    }
}