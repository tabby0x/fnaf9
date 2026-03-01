#include "LightScenarioManager.h"
#include "LightScenarioManagerSettings.h"
#include "LightScenarioAreaInfo.h"
#include "LightStreamingVolume.h"
#include "FNAFSaveData.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "PrecomputedVolumetricLightmap.h"
#include "Engine/MapBuildDataRegistry.h"

// Interval for incremental lighting propagation ticks (seconds)
static float GPropagateTickInterval = 0.1f;

ULightScenarioManager::ULightScenarioManager()
    : CurrentLightingScenario(ELightingScenario::None)
    , OldLightingScenario(ELightingScenario::None)
    , LoadedLightingScenario(ELightingScenario::None)
    , ScenarioArea(-1)
    , LoadedScenarioArea(-1)
    , bIsChangingScenario(false)
    , bIsWaitingOnLoad(false)
    , bIsUsingFade(false)
    , changeAreaState(EChangeAreaState::Idle)
    , levelWaitingOn(nullptr)
    , NextTickTime(0.0f)
    , NextPropagateTime(0.0f)
    , bVolumesGathered(false)
    , bScenarioAreaChange(false)
{
}

/*
 * Calls the standard PropagateLightingScenarioChange, which is slower than
 * Steel Wool's custom incremental version. If performance is an issue, we
 * can implement incremental propagation (iterating dirty primitives and
 * re-registering them with the renderer over multiple ticks).
 */
void ULightScenarioManager::PropagateLoadedLightingData()
{
    UWorld* World = GetWorld();
    if (!World) return;

    FString ScenarioName;
    switch (CurrentLightingScenario)
    {
    case ELightingScenario::LightsOn:  ScenarioName = TEXT("Day"); break;
    case ELightingScenario::LightsOff: ScenarioName = TEXT("Night"); break;
    case ELightingScenario::Dawn:      ScenarioName = TEXT("Dawn"); break;
    case ELightingScenario::DLC:       ScenarioName = TEXT("N_DLC"); break;
    default:                           ScenarioName = TEXT("Day"); break;
    }

    // DLC levels use MAP_LM_ which are streamed directly (handled by data table).
    // Base game levels use MAP_SLM_ which are editor-only, so we need the GUID trick.
    if (CurrentLightingScenario != ELightingScenario::DLC)
    {
        FString SLMPackagePath = FString::Printf(
            TEXT("/Game/Maps/World/Segmented_Light_Masters/MAP_SLM_%s_%.2d"),
            *ScenarioName, ScenarioArea);

        UPackage* SLMPackage = LoadPackage(nullptr, *SLMPackagePath, LOAD_NoWarn);

        if (SLMPackage)
        {
            UWorld* SLMWorld = nullptr;
            ForEachObjectWithPackage(SLMPackage, [&SLMWorld](UObject* Obj)
                {
                    if (UWorld* W = Cast<UWorld>(Obj))
                    {
                        SLMWorld = W;
                        return false;
                    }
                    return true;
                });

            if (SLMWorld && SLMWorld->PersistentLevel)
            {
                World->PersistentLevel->LevelBuildDataId =
                    SLMWorld->PersistentLevel->LevelBuildDataId;
            }
        }
    }
    // DLC: MAP_LM_DLC_XX is already streamed by the data table,
    // so its GUID is present in the world automatically.

    World->PropagateLightingScenarioChange();

    TSet<FSceneInterface*> Scenes;
    if (World->Scene) Scenes.Add(World->Scene);
    UpdateAllPrimitiveSceneInfosForScenes(Scenes);
}

/*
 * Original calls World->SetLightingScenariosLevelMap() which copies the
 * LightScenarioInfo TMap into a custom World member. We cannot replicate
 * this custom engine call, so we call PropagateLoadedLightingData() at
 * key transition points instead.
 */
void ULightScenarioManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    changeAreaState = EChangeAreaState::Idle;
    bScenarioAreaChange = false;
}

void ULightScenarioManager::Deinitialize()
{
    Super::Deinitialize();
}

bool ULightScenarioManager::IsTickable() const
{
    return true;
}

TStatId ULightScenarioManager::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULightScenarioManager, STATGROUP_Tickables);
}

/*
 * Throttles OnTick every 0.25s. Original also calls
 * PropagateLightingScenarioChangeIncremenatal when in PropagatingChanges
 * state, but since our propagation is synchronous, no incremental work
 * is needed.
 */
void ULightScenarioManager::Tick(float DeltaTime)
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        return;
    }

    float RealTime = World->GetRealTimeSeconds();

    if (RealTime >= NextTickTime)
    {
        NextTickTime = RealTime + 0.25f;
        OnTick();
    }
}

bool ULightScenarioManager::IsChangingScenario() const
{
    return bIsChangingScenario;
}

ELightingScenario ULightScenarioManager::GetCurrentLightingScenario() const
{
    return CurrentLightingScenario;
}

int32 ULightScenarioManager::GetCurrentArea() const
{
    return ScenarioArea;
}

/* DLC case uses "N_DLC" (not "DLC"), producing MAP_N_DLC_XX names. */
FName ULightScenarioManager::GetLevelNameFromAreaScenario(int32 Area, ELightingScenario Scenario)
{
    FString ScenarioName;

    switch (Scenario)
    {
    case ELightingScenario::LightsOn:
        ScenarioName = TEXT("Day");
        break;
    case ELightingScenario::LightsOff:
        ScenarioName = TEXT("Night");
        break;
    case ELightingScenario::Dawn:
        ScenarioName = TEXT("Dawn");
        break;
    case ELightingScenario::DLC:
        ScenarioName = TEXT("N_DLC");
        break;
    default:
        ScenarioName = TEXT("None");
        break;
    }

    FString MapName = FString::Printf(TEXT("MAP_%s_%.2d"), *ScenarioName, Area);
    return FName(*MapName);
}

void ULightScenarioManager::GetLightScenarioAreaFromMap(
    const FName& MapName,
    ELightScenarioArea& OutArea,
    ELightingScenario& OutScenario) const
{
    OutArea = ELightScenarioArea::None;
    OutScenario = ELightingScenario::None;

    const ULightScenarioManagerSettings* Settings = GetDefault<ULightScenarioManagerSettings>();
    if (!Settings)
    {
        return;
    }

    for (const auto& Pair : Settings->LightScenarioInfo)
    {
        if (Pair.Value.Dawn == MapName)
        {
            OutScenario = ELightingScenario::Dawn;
            OutArea = Pair.Key;
            return;
        }
        else if (Pair.Value.LightsOff == MapName)
        {
            OutScenario = ELightingScenario::LightsOff;
            OutArea = Pair.Key;
            return;
        }
        else if (Pair.Value.LightsOn == MapName)
        {
            OutScenario = ELightingScenario::LightsOn;
            OutArea = Pair.Key;
            return;
        }
    }
}

void ULightScenarioManager::SetInitialScenario(int32 Area, ELightingScenario Scenario)
{
    ScenarioArea = Area;
    ChangeScenario(Scenario, false);
}

void ULightScenarioManager::BeginLoadSequence()
{
    bIsWaitingOnLoad = true;
}

void ULightScenarioManager::EndLoadSequence()
{
    if (bIsWaitingOnLoad)
    {
        ELightingScenario Loaded = LoadedLightingScenario;
        bIsWaitingOnLoad = false;

        if (Loaded == CurrentLightingScenario)
        {
            ChangeArea(LoadedScenarioArea);
        }
        else
        {
            ChangeScenario(Loaded, false);
        }
    }
}

void ULightScenarioManager::ChangeScenario(ELightingScenario NewScenario, bool bUseFade)
{
    if (bIsWaitingOnLoad)
    {
        return;
    }

    if (bIsChangingScenario)
    {
        return;
    }

    bIsUsingFade = bUseFade;

    if (NewScenario == CurrentLightingScenario)
    {
        return;
    }

    OldLightingScenario = CurrentLightingScenario;
    bIsChangingScenario = true;
    CurrentLightingScenario = NewScenario;

    if (bIsUsingFade)
    {
        APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
        if (CameraManager)
        {
            CameraManager->StartCameraFade(0.0f, 1.0f, 0.7f, FLinearColor::Black, false, true);
        }

        UWorld* World = GetWorld();
        if (World)
        {
            FTimerHandle TimerHandle;
            FTimerDelegate TimerDelegate;
            TimerDelegate.BindUObject(this, &ULightScenarioManager::OnFadeoutFinished);
            World->GetTimerManager().SetTimer(TimerHandle, TimerDelegate, 0.7f, false);
        }
    }
    else
    {
        OnFadeoutFinished();
    }
}

/*
 * On initial load, OldLightingScenario is None, producing a level name like
 * "MAP_None_00" which doesn't exist. Trying to UnloadStreamLevel on a
 * non-existent level can deadlock the state machine since the latent action
 * callback may never fire, leaving bIsChangingScenario stuck true.
 */
void ULightScenarioManager::OnFadeoutFinished()
{
    if (OnBeginScenarioChange.IsBound())
    {
        OnBeginScenarioChange.Broadcast();
    }

    FName OldLevelName = GetLevelNameFromAreaScenario(ScenarioArea, OldLightingScenario);
    FName NewLevelName = GetLevelNameFromAreaScenario(ScenarioArea, CurrentLightingScenario);

    bool bShouldSkipUnload = false;

    // Skip if old scenario is None (initial load -- nothing to unload)
    if (OldLightingScenario == ELightingScenario::None)
    {
        bShouldSkipUnload = true;
    }

    if (OldLevelName == NewLevelName)
    {
        bShouldSkipUnload = true;
    }

    if (!bShouldSkipUnload)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            ULevelStreaming* OldStreaming = UGameplayStatics::GetStreamingLevel(World, OldLevelName);
            if (!OldStreaming || !OldStreaming->GetLoadedLevel())
            {
                bShouldSkipUnload = true;
            }
        }
    }

    if (bShouldSkipUnload)
    {
        OnUnloadFinished();
        return;
    }

    UWorld* World = GetWorld();
    if (World)
    {
        FLatentActionInfo LatentInfo;
        LatentInfo.CallbackTarget = this;
        LatentInfo.ExecutionFunction = FName("OnUnloadFinished");
        LatentInfo.UUID = GetUniqueID();
        LatentInfo.Linkage = 0;

        UGameplayStatics::UnloadStreamLevel(World, OldLevelName, LatentInfo, true);
    }
    else
    {
        bIsChangingScenario = false;
        changeAreaState = EChangeAreaState::Idle;
    }
}

void ULightScenarioManager::OnUnloadFinished()
{
    FName NewLevelName = GetLevelNameFromAreaScenario(ScenarioArea, CurrentLightingScenario);

    UWorld* World = GetWorld();
    if (World)
    {
        FLatentActionInfo LatentInfo;
        LatentInfo.CallbackTarget = this;
        LatentInfo.ExecutionFunction = FName("OnLoadFinished");
        LatentInfo.UUID = GetUniqueID();
        LatentInfo.Linkage = 0;

        UGameplayStatics::LoadStreamLevel(World, NewLevelName, true, true, LatentInfo);
    }
    else
    {
        bIsChangingScenario = false;
        changeAreaState = EChangeAreaState::Idle;
    }
}

/*
 * The original game's custom engine handles propagation automatically via
 * scenarioLevelsMap. Since we don't have the custom engine, we call
 * PropagateLoadedLightingData() to force re-initialization of level
 * rendering resources (MapBuildData / Volumetric Lightmap).
 */
void ULightScenarioManager::OnLoadFinished()
{
    if (bIsUsingFade)
    {
        APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
        if (CameraManager)
        {
            CameraManager->StartCameraFade(1.0f, 0.0f, 0.5f, FLinearColor::Black, false, false);
        }
    }

    PropagateLoadedLightingData();

    if (OnEndScenarioChange.IsBound())
    {
        OnEndScenarioChange.Broadcast();
    }

    bIsChangingScenario = false;
}

/*
 * When both levels are valid, the original calls
 * World->SetScenarioLevelsInCommon() (optimization, skipped) and
 * World->UseScenarioAreaChange(true). We emulate the latter with
 * bScenarioAreaChange = true.
 */
void ULightScenarioManager::ChangeArea(int32 Area)
{
    if (bIsWaitingOnLoad)
    {
        LoadedScenarioArea = Area;
        return;
    }

    if (bIsChangingScenario && changeAreaState >= EChangeAreaState::PropagatingChanges)
    {
        return;
    }

    int32 OldArea = ScenarioArea;
    if (Area == OldArea)
    {
        return;
    }

    bIsChangingScenario = true;
    OldLightingLevel = GetLevelNameFromAreaScenario(OldArea, CurrentLightingScenario);
    ScenarioArea = Area;
    CurrentLightScenarioLevel = GetLevelNameFromAreaScenario(Area, CurrentLightingScenario);

    UWorld* World = GetWorld();
    if (!World)
    {
        changeAreaState = EChangeAreaState::Idle;
        bIsChangingScenario = false;
        return;
    }

    bool bBothLevelsValid = (OldArea >= 0)
        && (CurrentLightingScenario != ELightingScenario::None)
        && (CurrentLightScenarioLevel != NAME_None)
        && (OldLightingLevel != NAME_None);

    if (bBothLevelsValid)
    {
        bScenarioAreaChange = true;

        if (changeAreaState >= EChangeAreaState::MakingOldInvisible)
        {
            // Already mid-transition, restart from MakingOldInvisible
            ULevelStreaming* OldLevel = UGameplayStatics::GetStreamingLevel(World, OldLightingLevel);
            levelWaitingOn = OldLevel;
            if (OldLevel)
            {
                changeAreaState = EChangeAreaState::MakingOldInvisible;
                OldLevel->SetShouldBeVisible(false);
            }
            else
            {
                bIsChangingScenario = false;
                changeAreaState = EChangeAreaState::Idle;
            }
        }
        else
        {
            CurrentLightScenarioLevel = GetLevelNameFromAreaScenario(Area, CurrentLightingScenario);
            changeAreaState = EChangeAreaState::LoadingMapBuildData;
            ULevelStreaming* NewLevel = UGameplayStatics::GetStreamingLevel(World, CurrentLightScenarioLevel);
            levelWaitingOn = NewLevel;
            if (NewLevel)
            {
                NewLevel->SetShouldBeLoaded(true);
                changeAreaState = EChangeAreaState::LoadingMapBuildData;
            }
            else
            {
                bIsChangingScenario = false;
                changeAreaState = EChangeAreaState::Idle;
            }
        }
    }
    else
    {
        // Initial load or invalid old area -- load the new level directly
        ULevelStreaming* NewLevel = UGameplayStatics::GetStreamingLevel(World, CurrentLightScenarioLevel);
        levelWaitingOn = NewLevel;
        if (NewLevel)
        {
            FLatentActionInfo LatentInfo;
            LatentInfo.CallbackTarget = this;
            LatentInfo.ExecutionFunction = FName("OnChangeAreaUnloadFinished");
            LatentInfo.UUID = GetUniqueID();
            LatentInfo.Linkage = 0;

            UGameplayStatics::LoadStreamLevel(this, CurrentLightScenarioLevel, true, true, LatentInfo);
        }
        else
        {
            changeAreaState = EChangeAreaState::Idle;
            bIsChangingScenario = false;
        }
    }
}

/* Checks bScenarioAreaChange (emulates World->scenarioAreaChange) to decide path. */
void ULightScenarioManager::OnChangeAreaLoadFinished()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        bIsChangingScenario = false;
        changeAreaState = EChangeAreaState::Idle;
        return;
    }

    if (bScenarioAreaChange)
    {
        changeAreaState = EChangeAreaState::MakingOldInvisible;
        ULevelStreaming* OldLevel = UGameplayStatics::GetStreamingLevel(World, OldLightingLevel);
        levelWaitingOn = OldLevel;
        if (OldLevel)
        {
            OldLevel->SetShouldBeVisible(false);
            return;
        }
    }
    else
    {
        changeAreaState = EChangeAreaState::MakingNewVisible;
        ULevelStreaming* NewLevel = UGameplayStatics::GetStreamingLevel(World, CurrentLightScenarioLevel);
        levelWaitingOn = NewLevel;
        if (NewLevel)
        {
            NewLevel->SetShouldBeVisible(true);
            return;
        }
    }

    bIsChangingScenario = false;
    changeAreaState = EChangeAreaState::Idle;
}

void ULightScenarioManager::OnChangeAreaUnloadFinished()
{
    bScenarioAreaChange = false;

    PropagateLoadedLightingData();

    changeAreaState = EChangeAreaState::Idle;
    bIsChangingScenario = false;
}

void ULightScenarioManager::OnPropagate()
{
    PropagateLoadedLightingData();
}

/*
 * Area change state machine. All World->scenarioAreaChange checks are
 * replaced with bScenarioAreaChange. PropagateLightingScenarioChangeInitial
 * is replaced with PropagateLoadedLightingData (synchronous level resource
 * re-init).
 */
void ULightScenarioManager::OnTick()
{
    // Lazy-gather volumes on first tick
    if (!bVolumesGathered)
    {
        bVolumesGathered = true;
        UWorld* World = GetWorld();
        if (World)
        {
            for (TActorIterator<ALightStreamingVolume> It(World); It; ++It)
            {
                LightStreamingVolumes.Add(*It);
            }
        }
    }

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
    if (PlayerPawn)
    {
        FVector ViewPoint = PlayerPawn->GetPawnViewLocation();

        TArray<int32> OverlappedAreas;
        for (const TWeakObjectPtr<ALightStreamingVolume>& WeakVolume : LightStreamingVolumes)
        {
            if (!WeakVolume.IsValid())
            {
                continue;
            }

            ALightStreamingVolume* Volume = WeakVolume.Get();
            if (Volume->EncompassesPoint(ViewPoint, 0.0f))
            {
                int32 VolumeArea = Volume->LightScenarioArea;
                OverlappedAreas.AddUnique(VolumeArea);
            }
        }

        // If current area is NOT in overlapped set, change to first found
        if (OverlappedAreas.Num() > 0 && !OverlappedAreas.Contains(ScenarioArea))
        {
            ChangeArea(OverlappedAreas[0]);
        }
    }

    if (!levelWaitingOn)
    {
        return;
    }

    UWorld* World = GetWorld();

    switch (changeAreaState)
    {
    case EChangeAreaState::LoadingMapBuildData:
    {
        if (!levelWaitingOn->GetLoadedLevel())
        {
            break;
        }

        if (World)
        {
            if (bScenarioAreaChange)
            {
                changeAreaState = EChangeAreaState::MakingOldInvisible;
                ULevelStreaming* OldLevel = UGameplayStatics::GetStreamingLevel(World, OldLightingLevel);
                levelWaitingOn = OldLevel;
                if (OldLevel)
                {
                    OldLevel->SetShouldBeVisible(false);
                    break;
                }
            }
            else
            {
                changeAreaState = EChangeAreaState::MakingNewVisible;
                ULevelStreaming* NewLevel = UGameplayStatics::GetStreamingLevel(World, CurrentLightScenarioLevel);
                levelWaitingOn = NewLevel;
                if (NewLevel)
                {
                    NewLevel->SetShouldBeVisible(true);
                    break;
                }
            }
        }

        bIsChangingScenario = false;
        changeAreaState = EChangeAreaState::Idle;
        break;
    }

    case EChangeAreaState::MakingOldInvisible:
    {
        if (levelWaitingOn->IsLevelVisible())
        {
            break;
        }

        if (World)
        {
            ULevelStreaming* NewLevel = UGameplayStatics::GetStreamingLevel(World, CurrentLightScenarioLevel);
            levelWaitingOn = NewLevel;
            if (NewLevel)
            {
                NewLevel->SetShouldBeVisible(true);
            }
            changeAreaState = EChangeAreaState::MakingNewVisible;
        }
        break;
    }

    case EChangeAreaState::MakingNewVisible:
    {
        if (!levelWaitingOn->IsLevelVisible())
        {
            break;
        }

        if (World)
        {
            ULevelStreaming* OldLevel = UGameplayStatics::GetStreamingLevel(World, OldLightingLevel);
            if (OldLevel)
            {
                PropagateLoadedLightingData();
                bScenarioAreaChange = false;
                changeAreaState = EChangeAreaState::PropagatingChanges;
            }
            else
            {
                OnChangeAreaUnloadFinished();
            }
            break;
        }

        bIsChangingScenario = false;
        changeAreaState = EChangeAreaState::Idle;
        break;
    }

    case EChangeAreaState::PropagatingChanges:
    {
        if (World)
        {
            /*
             * Original checks World->scenarioAreaChange, cleared when
             * actorsToPropagate is empty. Since PropagateLoadedLightingData
             * is synchronous, bScenarioAreaChange was already cleared in
             * MakingNewVisible, so we proceed immediately.
             */
            if (!bScenarioAreaChange)
            {
                ULevelStreaming* OldLevel = UGameplayStatics::GetStreamingLevel(World, OldLightingLevel);
                levelWaitingOn = OldLevel;
                if (OldLevel)
                {
                    OldLevel->SetShouldBeLoaded(false);
                }
                changeAreaState = EChangeAreaState::UnloadingOldLevel;
            }
        }
        break;
    }

    case EChangeAreaState::UnloadingOldLevel:
    {
        if (levelWaitingOn->GetLoadedLevel())
        {
            break;
        }

        bScenarioAreaChange = false;
        bIsChangingScenario = false;
        changeAreaState = EChangeAreaState::Idle;
        break;
    }

    default:
        break;
    }
}

void ULightScenarioManager::UnloadArea()
{
    int32 OldArea = ScenarioArea;
    bIsChangingScenario = true;
    OldLightingLevel = GetLevelNameFromAreaScenario(OldArea, CurrentLightingScenario);

    bool bCanDoSmoothTransition = (OldArea >= 0)
        && (CurrentLightingScenario != ELightingScenario::None)
        && (CurrentLightScenarioLevel != NAME_None)
        && (OldLightingLevel != NAME_None);

    if (bCanDoSmoothTransition)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            bScenarioAreaChange = true;

            changeAreaState = EChangeAreaState::MakingOldInvisible;
            ULevelStreaming* OldLevel = UGameplayStatics::GetStreamingLevel(World, OldLightingLevel);
            levelWaitingOn = OldLevel;
            if (OldLevel)
            {
                OldLevel->SetShouldBeVisible(false);
            }
        }
    }
    else
    {
        changeAreaState = EChangeAreaState::Idle;

        FLatentActionInfo LatentInfo;
        LatentInfo.CallbackTarget = this;
        LatentInfo.ExecutionFunction = FName("OnChangeAreaLoadFinished");
        LatentInfo.UUID = GetUniqueID();
        LatentInfo.Linkage = 0;

        UGameplayStatics::UnloadStreamLevel(this, OldLightingLevel, LatentInfo, false);
    }
}

void ULightScenarioManager::BindOnBeginScenarioChange(FOnLightScenarioChangeParam Delegate)
{
    OnBeginScenarioChange.AddUnique(Delegate);
}

void ULightScenarioManager::UnbindOnBeginScenarioChange(FOnLightScenarioChangeParam Delegate)
{
    OnBeginScenarioChange.Remove(Delegate);
}

void ULightScenarioManager::BindOnEndScenarioChange(FOnLightScenarioChangeParam Delegate)
{
    OnEndScenarioChange.AddUnique(Delegate);
}

void ULightScenarioManager::UnbindOnEndScenarioChange(FOnLightScenarioChangeParam Delegate)
{
    OnEndScenarioChange.Remove(Delegate);
}

void ULightScenarioManager::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (SaveDataObject)
    {
        SaveDataObject->LightScenarioManagerData.Scenario = CurrentLightingScenario;
    }
}

void ULightScenarioManager::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (SaveDataObject)
    {
        LoadedLightingScenario = SaveDataObject->LightScenarioManagerData.Scenario;
    }
}
