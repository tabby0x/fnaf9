#include "FNAFLevelManager.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingVolume.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "LightScenarioManager.h"
#include "StreamingLevelUtil.h"

static const FStreamingLevelArray EmptyLevelArray;

UFNAFLevelManager::UFNAFLevelManager()
    : CurrentPlayerPawn(nullptr)
    , CurrentPlayerComp(nullptr)
    , lightScenarioManager(nullptr)
    , bStreamingEnabled(true)
{
}

void UFNAFLevelManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Prepass to ensure streaming volumes are loaded/initialized
    TArray<AActor*> StreamingVolumes;
    UGameplayStatics::GetAllActorsOfClass(this, ALevelStreamingVolume::StaticClass(), StreamingVolumes);

    for (AActor* Actor : StreamingVolumes)
    {
        if (Actor)
        {
            ALevelStreamingVolume::StaticClass(); // Ensure CDO is loaded
        }
    }
}

void UFNAFLevelManager::Deinitialize()
{
    lightScenarioManager = nullptr;
    Super::Deinitialize();
}

TStatId UFNAFLevelManager::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UFNAFLevelManager, STATGROUP_Tickables);
}

void UFNAFLevelManager::Tick(float DeltaSeconds)
{
    if (!bStreamingEnabled)
    {
        TickSequentialMode();
    }
    else
    {
        TickStreamingMode();
    }
}

void UFNAFLevelManager::TickSequentialMode()
{
    bool bAnyLevelBecameVisible = false;

    // Pop levels from the back of the sequential queue that are already visible
    while (LevelsToLoadSequential.Num() > 0)
    {
        ULevelStreaming* LastLevel = LevelsToLoadSequential.Last();
        if (!LastLevel || !LastLevel->IsLevelVisible())
        {
            break;
        }
        LevelsToLoadSequential.Pop();
        bAnyLevelBecameVisible = true;
    }

    if (LevelsToLoadSequential.Num() > 0)
    {
        ULevelStreaming* LevelToProcess = LevelsToLoadSequential.Last();
        CurrentLoadedLevels.Add(LevelToProcess);
        CurrentVisibleLevels.Add(LevelToProcess);
    }

    for (ULevelStreaming* Level : CurrentLoadedLevels)
    {
        if (Level)
        {
            Level->SetShouldBeLoaded(true);
        }
    }

    for (ULevelStreaming* Level : CurrentVisibleLevels)
    {
        if (Level)
        {
            Level->SetShouldBeVisible(true);
        }
    }

    if (bAnyLevelBecameVisible)
    {
        if (OnlevelsUpdated.IsBound())
        {
            OnlevelsUpdated.Broadcast();
        }
    }
}

void UFNAFLevelManager::TickStreamingMode()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    if (!lightScenarioManager)
    {
        lightScenarioManager = World->GetSubsystem<ULightScenarioManager>();
    }

    // Skip tick while the light scenario is changing
    if (lightScenarioManager && lightScenarioManager->IsChangingScenario())
    {
        return;
    }

    // Clear cached level arrays for each streaming source
    for (auto& Pair : ComponentToLevelMap)
    {
        Pair.Value.Levels.Empty();
    }

    TSet<ULevelStreaming*> LevelsToLoad;
    TSet<ULevelStreaming*> LevelsVisible;

    TMap<ALevelStreamingVolume*, TArray<USceneComponent*>> StreamingVolumesProcessed;
    TMap<ALevelStreamingVolume*, FBoxSphereBounds> VolumeBoundsCache;

    const TArray<ULevelStreaming*>& WorldStreamingLevels = World->GetStreamingLevels();
    for (ULevelStreaming* StreamingLevel : WorldStreamingLevels)
    {
        if (!StreamingLevel)
        {
            continue;
        }

        const TArray<ALevelStreamingVolume*>& Volumes = StreamingLevel->EditorStreamingVolumes;
        for (ALevelStreamingVolume* Volume : Volumes)
        {
            if (!Volume || Volume->bDisabled)
            {
                continue;
            }

            if (!StreamingVolumesProcessed.Contains(Volume))
            {
                StreamingVolumesProcessed.Add(Volume);
                Volume->bEditorPreVisOnly = true;

                // Evaluate each registered streaming source against this volume
                for (int32 SourceIdx = 0; SourceIdx < StreamingSources.Num(); ++SourceIdx)
                {
                    USceneComponent* Source = StreamingSources[SourceIdx];
                    if (!Source)
                    {
                        continue;
                    }

                    FVector SourceLocation = Source->GetComponentLocation();

                    FBoxSphereBounds* CachedBounds = VolumeBoundsCache.Find(Volume);
                    if (!CachedBounds)
                    {
                        FBoxSphereBounds Bounds = Volume->GetBounds();
                        CachedBounds = &VolumeBoundsCache.Add(Volume, Bounds);
                    }

                    // Quick sphere distance check before expensive encompass test
                    float DistSq = FVector::DistSquared(SourceLocation, CachedBounds->Origin);
                    float RadiusSq = CachedBounds->SphereRadius * CachedBounds->SphereRadius;
                    if (DistSq >= RadiusSq)
                    {
                        continue;
                    }

                    if (!Volume->EncompassesPoint(SourceLocation))
                    {
                        continue;
                    }

                    StreamingVolumesProcessed[Volume].AddUnique(Source);

                    switch (Volume->StreamingUsage)
                    {
                    case SVB_LoadingAndVisibility:
                        LevelsToLoad.Add(StreamingLevel);
                        LevelsVisible.Add(StreamingLevel);

                        if (FStreamingLevelArray* LevelArray = ComponentToLevelMap.Find(Source))
                        {
                            LevelArray->Levels.Add(StreamingLevel);
                        }
                        break;

                    case SVB_Loading:
                    case SVB_VisibilityBlockingOnLoad:
                    default:
                        LevelsToLoad.Add(StreamingLevel);
                        break;
                    }
                }
            }
            else
            {
                // Volume already processed -- use cached source list
                TArray<USceneComponent*>& SourcesInVolume = StreamingVolumesProcessed[Volume];
                for (USceneComponent* Source : SourcesInVolume)
                {
                    switch (Volume->StreamingUsage)
                    {
                    case SVB_LoadingAndVisibility:
                        LevelsToLoad.Add(StreamingLevel);
                        LevelsVisible.Add(StreamingLevel);

                        if (FStreamingLevelArray* LevelArray = ComponentToLevelMap.Find(Source))
                        {
                            LevelArray->Levels.Add(StreamingLevel);
                        }
                        break;

                    case SVB_Loading:
                    case SVB_VisibilityBlockingOnLoad:
                    default:
                        LevelsToLoad.Add(StreamingLevel);
                        break;
                    }
                }
            }
        }
    }

    // Apply loading changes
    for (ULevelStreaming* Level : LevelsToLoad)
    {
        if (Level)
        {
            Level->SetShouldBeLoaded(true);
            CurrentLoadedLevels.Remove(Level);
        }
    }

    // Levels no longer needed: unload
    for (ULevelStreaming* Level : CurrentLoadedLevels)
    {
        if (Level)
        {
            Level->SetShouldBeLoaded(false);
        }
    }

    CurrentLoadedLevels = LevelsToLoad;

    // Apply visibility changes
    for (ULevelStreaming* Level : LevelsVisible)
    {
        if (Level)
        {
            Level->SetShouldBeVisible(true);
            CurrentVisibleLevels.Remove(Level);
        }
    }

    // Levels no longer visible: hide
    for (ULevelStreaming* Level : CurrentVisibleLevels)
    {
        if (Level)
        {
            Level->SetShouldBeVisible(false);
        }
    }

    CurrentVisibleLevels = LevelsVisible;

    if (OnlevelsUpdated.IsBound())
    {
        OnlevelsUpdated.Broadcast();
    }
}

// Level Add/Remove

void UFNAFLevelManager::AddLevelToLoad(ULevelStreaming* LevelToLoad)
{
    LevelsToLoadSequential.Add(LevelToLoad);
}

void UFNAFLevelManager::AddLevelArrayToLoad(const TArray<ULevelStreaming*>& LevelsToLoad)
{
    if (LevelsToLoad.Num() > 0)
    {
        LevelsToLoadSequential.Append(LevelsToLoad);
    }
}

void UFNAFLevelManager::AddLevelsFromStreamingSourceToLoad(const USceneComponent* StreamingSource)
{
    if (!StreamingSource)
    {
        return;
    }

    FVector SourceLocation = StreamingSource->GetComponentLocation();
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const TArray<ULevelStreaming*>& WorldStreamingLevels = World->GetStreamingLevels();
    for (ULevelStreaming* StreamingLevel : WorldStreamingLevels)
    {
        if (!StreamingLevel)
        {
            continue;
        }

        const TArray<ALevelStreamingVolume*>& Volumes = StreamingLevel->EditorStreamingVolumes;
        for (ALevelStreamingVolume* Volume : Volumes)
        {
            if (!Volume)
            {
                continue;
            }

            if (Volume->bDisabled || Volume->StreamingUsage != SVB_LoadingAndVisibility)
            {
                continue;
            }

            if (Volume->EncompassesPoint(SourceLocation))
            {
                LevelsToLoadSequential.Add(StreamingLevel);
            }
        }
    }
}

void UFNAFLevelManager::RemoveLevel(ULevelStreaming* LevelToUnload)
{
    CurrentLoadedLevels.Remove(LevelToUnload);
    CurrentVisibleLevels.Remove(LevelToUnload);
}

void UFNAFLevelManager::RemoveLevelArray(const TArray<ULevelStreaming*>& Levels)
{
    for (ULevelStreaming* Level : Levels)
    {
        CurrentLoadedLevels.Remove(Level);
        CurrentVisibleLevels.Remove(Level);
    }
}

void UFNAFLevelManager::RemoveAllLevels()
{
    CurrentLoadedLevels.Empty();
    CurrentVisibleLevels.Empty();
}

void UFNAFLevelManager::RemoveLevelsFromStreamingSource(const USceneComponent* StreamingSource)
{
    if (!StreamingSource)
    {
        return;
    }

    FVector SourceLocation = StreamingSource->GetComponentLocation();
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const TArray<ULevelStreaming*>& WorldStreamingLevels = World->GetStreamingLevels();
    for (ULevelStreaming* StreamingLevel : WorldStreamingLevels)
    {
        if (!StreamingLevel)
        {
            continue;
        }

        const TArray<ALevelStreamingVolume*>& Volumes = StreamingLevel->EditorStreamingVolumes;
        for (ALevelStreamingVolume* Volume : Volumes)
        {
            if (!Volume)
            {
                continue;
            }

            // Original only checks StreamingUsage (no bDisabled check)
            if (Volume->StreamingUsage != SVB_LoadingAndVisibility)
            {
                continue;
            }

            if (Volume->EncompassesPoint(SourceLocation))
            {
                CurrentLoadedLevels.Remove(StreamingLevel);
                CurrentVisibleLevels.Remove(StreamingLevel);
            }
        }
    }
}

// Streaming Source Registration

void UFNAFLevelManager::RegisterStreamingSource(USceneComponent* SceneComponent)
{
    if (!SceneComponent || !IsValid(SceneComponent))
    {
        return;
    }

    StreamingSources.AddUnique(SceneComponent);

    if (!ComponentToLevelMap.Contains(SceneComponent))
    {
        ComponentToLevelMap.Add(SceneComponent, FStreamingLevelArray());
    }
}

void UFNAFLevelManager::UnregisterStreamingSource(USceneComponent* SceneComponent)
{
    if (!SceneComponent || !IsValid(SceneComponent))
    {
        return;
    }

    StreamingSources.RemoveSingle(SceneComponent);
    ComponentToLevelMap.Remove(SceneComponent);
}

void UFNAFLevelManager::RegisterPawnStreamingSource(USceneComponent* SceneComponent)
{
    if (!SceneComponent)
    {
        return;
    }

    APawn* PawnOwner = Cast<APawn>(SceneComponent->GetOwner());
    if (!PawnOwner)
    {
        return;
    }

    PawnComponentMap.Add(PawnOwner, SceneComponent);

    // If this pawn is the current player pawn, register its component as a streaming source
    if (CurrentPlayerPawn == PawnOwner)
    {
        CurrentPlayerComp = SceneComponent;
        RegisterStreamingSource(SceneComponent);
    }
}

void UFNAFLevelManager::UnregisterPawnStreamingSource(USceneComponent* SceneComponent)
{
    if (!SceneComponent)
    {
        return;
    }

    APawn* PawnOwner = Cast<APawn>(SceneComponent->GetOwner());
    if (PawnOwner)
    {
        PawnComponentMap.Remove(PawnOwner);
    }

    if (CurrentPlayerComp == SceneComponent)
    {
        UnregisterStreamingSource(SceneComponent);
        CurrentPlayerComp = nullptr;
    }
}

// Player & Streaming State

void UFNAFLevelManager::SetPlayerPawn(APawn* PlayerPawn)
{
    if (CurrentPlayerComp)
    {
        UnregisterStreamingSource(CurrentPlayerComp);
        CurrentPlayerComp = nullptr;
    }

    if (USceneComponent** FoundComp = PawnComponentMap.Find(PlayerPawn))
    {
        if (*FoundComp)
        {
            RegisterStreamingSource(*FoundComp);
            CurrentPlayerComp = *FoundComp;
        }
    }

    CurrentPlayerPawn = PlayerPawn;
}

void UFNAFLevelManager::SetLevelStreamingEnable(bool bEnable)
{

    if (bEnable)
    {
        UStreamingLevelUtil::EnableAllStreamingVolumes(this, true);  // <- Is this line present?
    }

    bStreamingEnabled = bEnable;
}

bool UFNAFLevelManager::IsLevelStreamingEnable()
{
    return bStreamingEnabled;
}

// Queries

TArray<USceneComponent*> UFNAFLevelManager::GetStreamingSources() const
{
    return StreamingSources;
}

const FStreamingLevelArray& UFNAFLevelManager::GetLevelsForComponent(const USceneComponent* SceneComponent) const
{
    if (!SceneComponent || !IsValid(SceneComponent))
    {
        return EmptyLevelArray;
    }

    if (const FStreamingLevelArray* Found = ComponentToLevelMap.Find(const_cast<USceneComponent*>(SceneComponent)))
    {
        return *Found;
    }

    return EmptyLevelArray;
}

TArray<FName> UFNAFLevelManager::GetLevelNamesForComponent(const USceneComponent* SceneComponent) const
{
    TArray<FName> Result;

    const FStreamingLevelArray& LevelArray = GetLevelsForComponent(SceneComponent);

    Result.Reserve(LevelArray.Levels.Num());
    for (const TWeakObjectPtr<ULevelStreaming>& WeakLevel : LevelArray.Levels)
    {
        if (ULevelStreaming* Level = WeakLevel.Get())
        {
            Result.Add(Level->GetFName());
        }
    }

    return Result;
}
