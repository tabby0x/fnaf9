#include "LevelLoadSubsystem.h"
#include "DLCLevelSystemInfo.h"
#include "Engine/DataTable.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "LightScenarioManager.h"
#include "StreamingLevelUtil.h"

DEFINE_LOG_CATEGORY_STATIC(LogLevelLoad, Log, All);

ULevelLoadSubsystem::ULevelLoadSubsystem()
{
    // Member initialization order matches constructor pseudocode
    TableRowName = NAME_None;
    // LoadedLevels, UnloadedLevels default constructed empty
    // WaitingLevelsToLoad, WaitingLevelsToUnload default constructed empty
    QueuingUpAnotherLoad = false;
    LoadCount = 0;
    UnloadCount = 0;
    OldLevelsAreUnLoaded = false;
    NextLevelsAreVisible = false;
    CanTick = false;
    CurrentLevel = nullptr;
    LightScenario = 0;
    CurrentMapArea = EMapArea::Lobby;

    // Load the level system DataTable (static - shared across instances)
    static ConstructorHelpers::FObjectFinder<UDataTable> LevelDataObject(
        TEXT("DataTable'/Game/Data/LevelSystemData.LevelSystemData'"));
    if (LevelDataObject.Succeeded())
    {
        LevelSystemData = LevelDataObject.Object;
    }
    else
    {
        LevelSystemData = nullptr;
    }
}

TStatId ULevelLoadSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULevelLoadSubsystem, STATGROUP_Tickables);
}

bool ULevelLoadSubsystem::IsTickable() const
{
    return CanTick;
}

TArray<FName> ULevelLoadSubsystem::GetVisibleLevels()
{
    return LoadedLevels;
}

EMapArea ULevelLoadSubsystem::GetCurrentMapArea()
{
    return CurrentMapArea;
}

/*
 * Tick - Two-phase state machine: Unload old, change light scenario, load new.
 * Processes one level per tick in each phase, transitioning through
 * SetShouldBeVisible/SetShouldBeLoaded with wait states between each step.
 */
void ULevelLoadSubsystem::Tick(float DeltaTime)
{
    // Phase 1: Unload old levels (one per tick)
    if (!OldLevelsAreUnLoaded)
    {
        if (UnloadCount > UnloadedLevels.Num() - 1)
        {
            // All old levels have been fully unloaded
            OldLevelsAreUnLoaded = true;

            // Trigger light scenario area change now that old levels are gone
            UWorld* World = GetWorld();
            if (World)
            {
                ULightScenarioManager* LSM = World->GetSubsystem<ULightScenarioManager>();
                if (LSM)
                {
                    LSM->ChangeArea(LightScenario);
                }
            }
        }
        else
        {
            // Process the current level to unload
            UWorld* World = GetWorld();
            if (World)
            {
                CurrentLevel = UGameplayStatics::GetStreamingLevel(World, UnloadedLevels[UnloadCount]);
            }

            if (!CurrentLevel)
            {
                // Level not found in world, skip it
                UnloadCount++;
            }
            else
            {
                // Step 1: Request hide
                CurrentLevel->SetShouldBeVisible(false);

                // Step 2: Once hidden, request unload
                if (!CurrentLevel->IsLevelVisible())
                {
                    UStreamingLevelUtil::ClearStandaloneFlagsForLoadedLevel(CurrentLevel);
                    CurrentLevel->SetShouldBeLoaded(false);
                }

                // Step 3: Once fully unloaded (LoadedLevel is null), advance
                if (!CurrentLevel->GetLoadedLevel())
                {
                    UnloadCount++;
                }
            }
        }
    }

    // Gate: wait for light scenario transition to finish
    bool bLightScenarioChanging = false;
    {
        UWorld* World = GetWorld();
        if (World)
        {
            ULightScenarioManager* LSM = World->GetSubsystem<ULightScenarioManager>();
            if (LSM)
            {
                bLightScenarioChanging = LSM->IsChangingScenario();
            }
        }
    }

    // Phase 2: Load new levels (one per tick, only when unload done and lights stable)
    if (!NextLevelsAreVisible && OldLevelsAreUnLoaded && !bLightScenarioChanging)
    {
        if (LoadCount > LoadedLevels.Num() - 1)
        {
            // All levels in the LoadedLevels array have been processed
            if (QueuingUpAnotherLoad)
            {
                // Another area was queued via FindNextAreaInTable while we were loading.
                // The new entries were appended to LoadedLevels/UnloadedLevels.
                // Reset counters to process from the beginning — already-processed
                // levels will be quickly skipped (already loaded/unloaded).
                LoadCount = 0;
                UnloadCount = 0;
                OldLevelsAreUnLoaded = false;
                NextLevelsAreVisible = false;
                QueuingUpAnotherLoad = false;
            }
            else
            {
                // Fully complete — all levels loaded and visible
                NextLevelsAreVisible = true;
                LoadCompleted.Broadcast();
                CanTick = false;
            }
        }
        else
        {
            // Process the current level to load
            UWorld* World = GetWorld();
            if (World)
            {
                CurrentLevel = UGameplayStatics::GetStreamingLevel(World, LoadedLevels[LoadCount]);
            }

            if (!CurrentLevel)
            {
                // Level not found in world, skip it
                LoadCount++;
            }
            else
            {
                // Step 1: Request load
                CurrentLevel->SetShouldBeLoaded(true);

                // Step 2: Once loaded, request visible
                if (CurrentLevel->GetLoadedLevel())
                {
                    CurrentLevel->SetShouldBeVisible(true);
                }

                // Step 3: Once visible, advance
                if (CurrentLevel->IsLevelVisible())
                {
                    LoadCount++;
                }
            }
        }
    }
}

/*
 * LoadTheNextArea - Maps EMapArea enum to DataTable row name and kicks off loading.
 * Uses a static TMap initialized once. The 52-entry enum-to-name mapping is hardcoded
 * in the binary. BonnieBowl(40) and UnloadRacewayPartOne(39) are out-of-order in
 * the original binary but the TMap handles this correctly since it's keyed by enum value.
 */
void ULevelLoadSubsystem::LoadTheNextArea(EMapArea MapArea)
{
    UE_LOG(LogLevelLoad, Log, TEXT("LoadTheNextArea called, MapArea: %d"), (int32)MapArea);

    // Static TMap built once from initializer list
    static TMap<EMapArea, FName> MapAreaRowMapping = {
        { EMapArea::Lobby, FName("Lobby") },
        { EMapArea::LobbySecondFloor, FName("LobbySecondFloor") },
        { EMapArea::LobbyElevatorUnload, FName("LobbyElevatorUnload") },
        { EMapArea::LobbyElevatorTop, FName("LobbyElevatorTop") },
        { EMapArea::FoodCourt, FName("FoodCourt") },
        { EMapArea::FCPipeToGG, FName("FCPipeToGG") },
        { EMapArea::GatorGolfStomachRoom, FName("GatorGolfStomachRoom") },
        { EMapArea::GatorGolfBathrooms, FName("GatorGolfBathrooms") },
        { EMapArea::GatorGolfMontyCamera, FName("GatorGolfMontyCamera") },
        { EMapArea::GatorGolfStairs, FName("GatorGolfStairs") },
        { EMapArea::GatorGolfMainArea, FName("GatorGolfMainArea") },
        { EMapArea::GatorGolfKitchen, FName("GatorGolfKitchen") },
        { EMapArea::GatorGolfBackHall, FName("GatorGolfBackHall") },
        { EMapArea::GatorGolfBackOffice, FName("GatorGolfBackOffice") },
        { EMapArea::DaycareLoadWarehouse, FName("DaycareLoadWarehouse") },
        { EMapArea::DaycareTransitionRoom, FName("DaycareTransitionRoom") },
        { EMapArea::UnloadGolfAtDCEntrance, FName("UnloadGolfAtDCEntrance") },
        { EMapArea::DaycarePlayArea, FName("DaycarePlayArea") },
        { EMapArea::DaycareBallPitEntrance, FName("DaycareBallPitEntrance") },
        { EMapArea::DaycareBallPitSlide, FName("DaycareBallPitSlide") },
        { EMapArea::DaycareEclipseTransition, FName("DaycareEclipseTransition") },
        { EMapArea::DaycarePlayAreaExit, FName("DaycarePlayAreaExit") },
        { EMapArea::DaycareTheater, FName("DaycareTheater") },
        { EMapArea::DaycareUnloadPlayArea, FName("DaycareUnloadPlayArea") },
        { EMapArea::CatwalksVent, FName("CatwalksVent") },
        { EMapArea::MontyRide, FName("MontyRide") },
        { EMapArea::CatwalksMainArea, FName("CatwalksMainArea") },
        { EMapArea::CatwalksSectionThree, FName("CatwalksSectionThree") },
        { EMapArea::CupcakesPrepRoom, FName("CupcakesPrepRoom") },
        { EMapArea::Cupcakes, FName("Cupcakes") },
        { EMapArea::CupcakesUnloadPrepRoom, FName("CupcakesUnloadPrepRoom") },
        { EMapArea::CupcakesPrepToMain, FName("CupcakesPrepToMain") },
        { EMapArea::CupcakesBackRoom, FName("CupcakesBackRoom") },
        { EMapArea::ServerRoom, FName("ServerRoom") },
        { EMapArea::Salon, FName("Salon") },
        { EMapArea::SalonLogRide, FName("SalonLogRide") },
        { EMapArea::LogRide, FName("LogRide") },
        { EMapArea::UnloadLogRide, FName("UnloadLogRide") },
        { EMapArea::RacewayPartOne, FName("RacewayPartOne") },
        { EMapArea::BonnieBowl, FName("BonnieBowl") },
        { EMapArea::UnloadRacewayPartOne, FName("UnloadRacewayPartOne") },
        { EMapArea::FazerBlastOffice, FName("FazerBlastOffice") },
        { EMapArea::FazerBlastOfficeVent, FName("FazerBlastOfficeVent") },
        { EMapArea::FazerBlastOfficeCatwalks, FName("FazerBlastOfficeCatwalks") },
        { EMapArea::FazerBlastMainArea, FName("FazerBlastMainArea") },
        { EMapArea::FazerblastExitVent, FName("FazerblastExitVent") },
        { EMapArea::FazerBlastTransition, FName("FazerBlastTransition") },
        { EMapArea::RacewayPartTwoDoors, FName("RacewayPartTwoDoors") },
        { EMapArea::RacewayPartTwoGarages, FName("RacewayPartTwoGarages") },
        { EMapArea::RacewayUnloadTransitionRoom, FName("RacewayUnloadTransitionRoom") },
        { EMapArea::RacewayPartTwo, FName("RacewayPartTwo") },
        { EMapArea::Sinkhole, FName("Sinkhole") },
    };

    const FName* RowName = MapAreaRowMapping.Find(MapArea);
    if (!RowName)
    {
        return;
    }

    TableRowName = *RowName;
    CurrentMapArea = MapArea;
    FindNextAreaInTable(*RowName);
}

/*
 * FindNextAreaInTable - Reads a DataTable row and configures load/unload state.
 * If no load in progress, replaces arrays and starts fresh.
 * If a load is in progress, appends to arrays and sets the queuing flag.
 */
void ULevelLoadSubsystem::FindNextAreaInTable(FName NextRowName)
{
    if (!LevelSystemData)
    {
        return;
    }

    // Verify the DataTable row struct is compatible
    if (!LevelSystemData->GetRowStruct())
    {
        return;
    }

    UScriptStruct* ExpectedStruct = FDLCLevelSystemInfo::StaticStruct();
    if (!ExpectedStruct)
    {
        return;
    }

    if (!LevelSystemData->GetRowStruct()->IsChildOf(ExpectedStruct))
    {
        return;
    }

    if (NextRowName.IsNone())
    {
        return;
    }

    // Look up the row in the DataTable
    static const FString ContextString(TEXT("LevelLoadSubsystem"));
    FDLCLevelSystemInfo* RowData = LevelSystemData->FindRow<FDLCLevelSystemInfo>(NextRowName, ContextString, false);
    if (!RowData)
    {
        return;
    }

    if (CanTick)
    {
        // A load is already in progress — append the new levels and flag for re-processing
        LoadedLevels.Append(RowData->LevelsToLoad);
        UnloadedLevels.Append(RowData->LevelsToUnload);
        LightScenario = RowData->LightScenario;
        QueuingUpAnotherLoad = true;
    }
    else
    {
        // No load in progress — start fresh
        LoadedLevels = RowData->LevelsToLoad;
        UnloadedLevels = RowData->LevelsToUnload;
        LightScenario = RowData->LightScenario;
        OldLevelsAreUnLoaded = false;
        NextLevelsAreVisible = false;
        LoadCount = 0;
        UnloadCount = 0;
        CanTick = true;
    }
}