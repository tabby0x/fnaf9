#include "LevelLoaderComponent.h"
#include "LightScenarioManager.h"
#include "ChowdaDebugSubsystem.h"
#include "Engine/LevelStreaming.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "StreamingLevelUtil.h"

ULevelLoaderComponent::ULevelLoaderComponent()
{
    UnloadScenarioOnly = false;
    ChowdaDebuger = nullptr;
    CurrentLevel = nullptr;
    LightingMap = nullptr;
    LightScenarioManager = nullptr;
    NextLightScenario = 0;
    LoadCount = 0;
    UnloadCount = 0;
    OldLevelsAreUnLoaded = false;
    NextLevelsAreVisible = false;
    LightMapToUnload = NAME_None;
    LightMapToLoad = NAME_None;

    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void ULevelLoaderComponent::BeginPlay()
{
    Super::BeginPlay();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    LightScenarioManager = World->GetSubsystem<ULightScenarioManager>();
    ChowdaDebuger = World->GetSubsystem<UChowdaDebugSubsystem>();
}

/*
 * Sequential unload -> area change -> sequential load, one level per tick.
 * Phase 1: hide + unload each level in LevelsToUnload. When done, call ChangeArea.
 * Phase 2: wait for lighting change, then load + show each level in LevelsToLoad.
 */
void ULevelLoaderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    UWorld* World = GetWorld();

    // Phase 1: Unload old levels
    if (!OldLevelsAreUnLoaded)
    {
        if (UnloadCount > LevelsToUnload.Num() - 1)
        {
            OldLevelsAreUnLoaded = true;
            if (LightScenarioManager)
            {
                LightScenarioManager->ChangeArea(NextLightScenario);
            }
        }
        else
        {
            ULevelStreaming* StreamingLevel = UGameplayStatics::GetStreamingLevel(World, LevelsToUnload[UnloadCount]);
            CurrentLevel = StreamingLevel;

            if (!StreamingLevel)
            {
                ++UnloadCount;
            }
            else
            {
                StreamingLevel->SetShouldBeVisible(false);

                if (!CurrentLevel->IsLevelVisible())
                {
                    UStreamingLevelUtil::ClearStandaloneFlagsForLoadedLevel(CurrentLevel);
                    CurrentLevel->SetShouldBeLoaded(false);
                }

                if (!CurrentLevel->GetLoadedLevel())
                {
                    ++UnloadCount;
                }
            }
        }
    }

    // Phase 2: Load new levels (only after unload complete and lighting scenario change done)
    if (!NextLevelsAreVisible && OldLevelsAreUnLoaded && LightScenarioManager && !LightScenarioManager->bIsChangingScenario)
    {
        if (LoadCount > LevelsToLoad.Num() - 1)
        {
            NextLevelsAreVisible = true;
            SetComponentTickEnabled(false);
        }
        else
        {
            ULevelStreaming* StreamingLevel = UGameplayStatics::GetStreamingLevel(World, LevelsToLoad[LoadCount]);
            CurrentLevel = StreamingLevel;

            if (!StreamingLevel)
            {
                ++LoadCount;
            }
            else
            {
                StreamingLevel->SetShouldBeLoaded(true);

                if (CurrentLevel->GetLoadedLevel())
                {
                    CurrentLevel->SetShouldBeVisible(true);
                }

                if (CurrentLevel->IsLevelVisible())
                {
                    ++LoadCount;
                }
            }
        }
    }
}

void ULevelLoaderComponent::LoadTheNextArea()
{
    OldLevelsAreUnLoaded = false;
    NextLevelsAreVisible = false;
    LoadCount = 0;
    UnloadCount = 0;
    SetComponentTickEnabled(true);
}

// TODO: restore from pseudocode if available
void ULevelLoaderComponent::LoadTheNextScenario()
{
}

// TODO: restore from pseudocode if available
void ULevelLoaderComponent::UnLoadCurrentScenario()
{
}

void ULevelLoaderComponent::DebugSkipToNextArea()
{
    if (ChowdaDebuger)
    {
        LevelsToUnload = ChowdaDebuger->VisibleLevels;
        LevelsToLoad = ChowdaDebuger->NewAreaLevels;
        NextLightScenario = ChowdaDebuger->NewLightScenario;
    }

    OldLevelsAreUnLoaded = false;
    NextLevelsAreVisible = false;
    LoadCount = 0;
    UnloadCount = 0;
    SetComponentTickEnabled(true);
}

TArray<FName> ULevelLoaderComponent::GetLevelsToLoad()
{
    return LevelsToLoad;
}

TArray<FName> ULevelLoaderComponent::SetLevelsToLoad(TArray<FName> LoadedLevels)
{
    LevelsToLoad = MoveTemp(LoadedLevels);
    return LevelsToLoad;
}

TArray<FName> ULevelLoaderComponent::SetLevelsToUnLoad(TArray<FName> UnloadedLevels)
{
    LevelsToUnload = MoveTemp(UnloadedLevels);
    return LevelsToUnload;
}

int32 ULevelLoaderComponent::SetNextLightScenario(int32 Scenario)
{
    NextLightScenario = Scenario;
    return Scenario;
}
