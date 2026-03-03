#include "StreamingLevelUtil.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingVolume.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "LatentActions.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogStreamingLevel, Log, All);

/*
 * FStreamAllLevelsAction - Latent action that loads streaming levels one by one.
 * Created by LoadStreamingLevelsAtLocation. Each tick checks if the current level
 * has finished loading, then advances to the next. When all are loaded, optionally
 * re-enables streaming volumes and completes the latent action.
 */
class FStreamAllLevelsAction : public FPendingLatentAction
{
public:
    UWorld* World;
    TArray<FName> LevelNames;
    bool bEnableVolumesAfterLoad;
    int32 CurrentIndex;
    ULevelStreaming* CurrentLoadingLevel;

    FName ExecutionFunction;
    int32 OutputLink;
    FWeakObjectPtr CallbackTarget;

    FStreamAllLevelsAction(UWorld* InWorld, const TArray<FName>& InLevelNames, bool bInEnableVolumes, const FLatentActionInfo& LatentInfo)
        : World(InWorld)
        , LevelNames(InLevelNames)
        , bEnableVolumesAfterLoad(bInEnableVolumes)
        , CurrentIndex(0)
        , CurrentLoadingLevel(nullptr)
        , ExecutionFunction(LatentInfo.ExecutionFunction)
        , OutputLink(LatentInfo.Linkage)
        , CallbackTarget(LatentInfo.CallbackTarget)
    {
        LoadNextLevel();
    }

    void LoadNextLevel()
    {
        CurrentLoadingLevel = nullptr;

        while (CurrentIndex < LevelNames.Num())
        {
            ULevelStreaming* Level = UStreamingLevelUtil::FindAndCacheLevelStreamingObject(
                LevelNames[CurrentIndex], World);

            if (Level)
            {
                Level->SetShouldBeLoaded(true);
                CurrentLoadingLevel = Level;
                return;
            }

            // Level not found, skip it
            CurrentIndex++;
        }
    }

    virtual void UpdateOperation(FLatentResponse& Response) override
    {
        if (CurrentIndex >= LevelNames.Num())
        {
            // All levels loaded — optionally re-enable streaming volumes
            if (bEnableVolumesAfterLoad)
            {
                UStreamingLevelUtil::EnableAllStreamingVolumes(World, true);
            }
            Response.FinishAndTriggerIf(true, ExecutionFunction, OutputLink, CallbackTarget);
            return;
        }

        // Check if current level has finished loading
        if (CurrentLoadingLevel && CurrentLoadingLevel->GetLoadedLevel())
        {
            CurrentIndex++;
            LoadNextLevel();
        }
    }

#if WITH_EDITOR
    virtual FString GetDescription() const override
    {
        return FString::Printf(TEXT("StreamAllLevels: %d/%d"), CurrentIndex, LevelNames.Num());
    }
#endif
};

UStreamingLevelUtil::UStreamingLevelUtil()
{
}

void UStreamingLevelUtil::ClearStandaloneFlagsForLoadedLevel(ULevelStreaming* StreamingLevel)
{
    if (!StreamingLevel)
    {
        return;
    }

    UObject* LoadedLevelObject = StreamingLevel->GetLoadedLevel();
    if (!LoadedLevelObject)
    {
        return;
    }

    UWorld* LoadedLevelWorld = Cast<UWorld>(LoadedLevelObject->GetOuter());
    if (!LoadedLevelWorld)
    {
        return;
    }

    UPackage* LoadedLevelPackage = LoadedLevelWorld->GetOutermost();
    if (!LoadedLevelPackage)
    {
        return;
    }

    LoadedLevelPackage->ClearFlags(RF_Standalone);
    LoadedLevelWorld->ClearFlags(RF_Standalone);

    ForEachObjectWithPackage(LoadedLevelPackage, [](UObject* PackageObject)
        {
            if (PackageObject)
            {
                PackageObject->ClearFlags(RF_Standalone);
            }
            return true;
        });
}

// Finds a ULevelStreaming by matching the end of its package name.
// Short package names get "/" prepended so "MyLevel" matches ".../MyLevel".
ULevelStreaming* UStreamingLevelUtil::FindAndCacheLevelStreamingObject(FName LevelName, UWorld* InWorld)
{
    if (LevelName.IsNone())
    {
        return nullptr;
    }

    // Convert to a searchable package name string
    FString SearchName = LevelName.ToString();

    // If it's a short package name (no path separators), prepend "/" for EndsWith matching
    if (FPackageName::IsShortPackageName(SearchName))
    {
        SearchName = FString(TEXT("/")) + SearchName;
    }

    // Search all streaming levels for one whose package name ends with our search string
    for (ULevelStreaming* StreamingLevel : InWorld->GetStreamingLevels())
    {
        if (StreamingLevel)
        {
            const FString PackageName = StreamingLevel->GetWorldAssetPackageName();
            if (PackageName.EndsWith(SearchName, ESearchCase::IgnoreCase))
            {
                return StreamingLevel;
            }
        }
    }

    return nullptr;
}

// Sets or clears bDisabled on every ALevelStreamingVolume in the world.
void UStreamingLevelUtil::EnableAllStreamingVolumes(const UObject* WorldContextObject, bool bEnable)
{

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        return;
    }

    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(WorldContextObject, ALevelStreamingVolume::StaticClass(), VolumeActors);

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (Volume)
        {
            Volume->bDisabled = !bEnable;
        }
    }
}

TArray<ULevel*> UStreamingLevelUtil::GetAllLevels(const UObject* WorldContextObject)
{
    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        return TArray<ULevel*>();
    }

    return World->GetLevels();
}

TArray<ULevelStreaming*> UStreamingLevelUtil::GetAllStreamingLevels(const UObject* WorldContextObject)
{
    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        return TArray<ULevelStreaming*>();
    }

    return World->GetStreamingLevels();
}

TArray<ALevelStreamingVolume*> UStreamingLevelUtil::GetAllStreamingVolumes(const UObject* WorldContextObject)
{
    TArray<ALevelStreamingVolume*> Result;

    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(WorldContextObject, ALevelStreamingVolume::StaticClass(), VolumeActors);

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (Volume)
        {
            Result.Add(Volume);
        }
    }

    return Result;
}

// Returns all ALevelStreamingVolume* that encompass the given world position.
TArray<ALevelStreamingVolume*> UStreamingLevelUtil::GetAllStreamingVolumesAtLocation(
    const UObject* WorldContextObject, const FVector& WorldLocation)
{
    TArray<ALevelStreamingVolume*> Result;

    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(WorldContextObject, ALevelStreamingVolume::StaticClass(), VolumeActors);

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (Volume)
        {
            if (Volume->EncompassesPoint(WorldLocation, 0.0f))
            {
                Result.Add(Volume);
            }
        }
    }

    return Result;
}

// Finds all streaming volumes at the location, then resolves their associated
// streaming level names to actual ULevelStreaming objects.
TArray<ULevelStreaming*> UStreamingLevelUtil::GetAllStreamingLevelsAtLocation(
    const UObject* WorldContextObject, const FVector& WorldLocation)
{
    TArray<ULevelStreaming*> Result;

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        return Result;
    }

    // First, find all streaming volumes that contain the location
    TArray<ALevelStreamingVolume*> VolumesAtLocation;

    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(WorldContextObject, ALevelStreamingVolume::StaticClass(), VolumeActors);

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (Volume && Volume->EncompassesPoint(WorldLocation, 0.0f))
        {
            VolumesAtLocation.Add(Volume);
        }
    }

    // For each volume, resolve its StreamingLevelNames to ULevelStreaming objects
    for (ALevelStreamingVolume* Volume : VolumesAtLocation)
    {
        for (const FName& LevelName : Volume->StreamingLevelNames)
        {
            ULevelStreaming* StreamingLevel = FindAndCacheLevelStreamingObject(LevelName, World);
            if (StreamingLevel)
            {
                Result.Add(StreamingLevel);
            }
        }
    }

    return Result;
}

/*
 * LoadStreamingLevelsAtLocation - Latent Blueprint action that disables all
 * streaming volumes, finds volumes at the target location, collects their level
 * names, then loads them one by one. This is the game's mechanism for pre-loading
 * an area before the player arrives (loading screens, teleportation, respawn).
 */
void UStreamingLevelUtil::LoadStreamingLevelsAtLocation(
    const UObject* WorldContextObject,
    const FVector& WorldLocation,
    bool bEnableVolumesAfterLoad,
    FLatentActionInfo LatentInfo)
{
    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        UE_LOG(LogStreamingLevel, Error, TEXT("LoadStreamingLevelsAtLocation: Invalid WorldContextObject"));
        return;
    }

    FLatentActionManager& LatentActionManager = World->GetLatentActionManager();

    if (LatentActionManager.FindExistingAction<FStreamAllLevelsAction>(LatentInfo.CallbackTarget, LatentInfo.UUID))
    {
        UE_LOG(LogStreamingLevel, Warning, TEXT("LoadStreamingLevelsAtLocation: Duplicate UUID, skipping"));
        return;
    }

    // Gather all streaming volume actors
    TArray<AActor*> VolumeActors;
    UGameplayStatics::GetAllActorsOfClass(WorldContextObject, ALevelStreamingVolume::StaticClass(), VolumeActors);

    // Collect level names from volumes at the target location
    // Also DISABLE every volume (the game disables all volumes before loading)
    TArray<FName> LevelsToLoad;

    for (AActor* Actor : VolumeActors)
    {
        ALevelStreamingVolume* Volume = Cast<ALevelStreamingVolume>(Actor);
        if (!Volume)
        {
            continue;
        }

        // Disable this volume
        Volume->bDisabled = true;

        // If the volume contains the target location, collect its level names
        if (Volume->EncompassesPoint(WorldLocation, 0.0f))
        {
            for (const FName& LevelName : Volume->StreamingLevelNames)
            {
                LevelsToLoad.Add(LevelName);
            }
        }
    }

    // Create the latent action to load levels one by one
    FStreamAllLevelsAction* Action = new FStreamAllLevelsAction(
        World, LevelsToLoad, bEnableVolumesAfterLoad, LatentInfo);

    LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, Action);
}