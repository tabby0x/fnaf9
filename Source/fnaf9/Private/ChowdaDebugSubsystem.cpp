/* Debug subsystem for RUIN (DLC/Chowda). Provides level-skip and item
   award functionality for development.

   NOTE: Requires TableRowName in ULevelLoadSubsystem to be accessible.
   Either add a getter or friend class UChowdaDebugSubsystem. */

#include "ChowdaDebugSubsystem.h"
#include "DLCLevelSystemInfo.h"
#include "LevelLoadSubsystem.h"
#include "AIDLC_RabbitSystem.h"
#include "TeleportationSubsystem.h"
#include "WorldStateSystem.h"
#include "FNAFInventorySystem.h"
#include "Engine/DataTable.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

UChowdaDebugSubsystem::UChowdaDebugSubsystem()
{
    LevelLoader = nullptr;
    LevelSystemData = nullptr;
    CurrentLocation = NAME_None;
    NewLightScenario = 0;

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

// Unloads all non-DLC streaming levels, then loads the target area and awards relevant items
void UChowdaDebugSubsystem::GoToThisArea(EMapArea MapArea)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Unload anything that's NOT DLC
    const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
    for (ULevelStreaming* Level : StreamingLevels)
    {
        if (!Level)
        {
            continue;
        }

        FString PackageName = Level->GetWorldAssetPackageName();
        if (!PackageName.Contains(TEXT("N_DLC")))
        {
            Level->SetShouldBeLoaded(false);
        }
    }

    LevelLoader = World->GetSubsystem<ULevelLoadSubsystem>();
    if (LevelLoader)
    {
        LevelLoader->LoadTheNextArea(MapArea);
        AwardRelevantItems(LevelLoader->GetTableRowName());
    }
}

/* Configures AIDLC_RabbitSystem with alert types and triggers AR world
   entry if player is in AR. */
void UChowdaDebugSubsystem::SpawnDLCRabbit(TArray<FAnimatronicTypeData> TypesToAlertIn)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UAIDLC_RabbitSystem* RabbitSystem = World->GetSubsystem<UAIDLC_RabbitSystem>();
    UTeleportationSubsystem* TeleportSys = World->GetSubsystem<UTeleportationSubsystem>();

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return;
    }

    UWorldStateSystem* WorldStateSys = GameInstance->GetSubsystem<UWorldStateSystem>();

    if (!RabbitSystem)
    {
        return;
    }

    RabbitSystem->SetTypesToAlert(TypesToAlertIn);

    if (WorldStateSys)
    {
        WorldStateSys->AddActivated(FName(TEXT("DLC_RabbitTeleportSpawn")));
    }

    if (TeleportSys && !TeleportSys->IsPlayerInNormal())
    {
        RabbitSystem->OnARWorldEntry();
    }
}

/* Clears ALL inventory, then awards items from the target area's DependentAreas,
   giving the player exactly the inventory state they would have at that point
   in a normal playthrough. */
void UChowdaDebugSubsystem::AwardRelevantItems(FName AreaRowName)
{
    UWorld* World = GetWorld();
    if (!World || !LevelSystemData)
    {
        return;
    }

    if (!LevelSystemData->GetRowStruct() ||
        !LevelSystemData->GetRowStruct()->IsChildOf(FDLCLevelSystemInfo::StaticStruct()))
    {
        return;
    }

    static const FString ContextString(TEXT("AwardRelevantItems"));
    FDLCLevelSystemInfo* TargetRow = LevelSystemData->FindRow<FDLCLevelSystemInfo>(
        AreaRowName, ContextString, false);

    TArray<FName> DependentAreas;
    if (TargetRow)
    {
        DependentAreas = TargetRow->DependentAreas;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        return;
    }

    UFNAFInventorySystem* InventorySys = GameInstance->GetSubsystem<UFNAFInventorySystem>();
    if (!InventorySys)
    {
        return;
    }

    // Phase 1: Remove ALL items from ALL rows
    TArray<FName> RowNames = LevelSystemData->GetRowNames();

    for (int32 i = 0; i <= RowNames.Num() - 1; ++i)
    {
        FDLCLevelSystemInfo* Row = LevelSystemData->FindRow<FDLCLevelSystemInfo>(
            RowNames[i], ContextString, false);
        if (!Row)
        {
            continue;
        }

        for (int32 j = 0; j < Row->ItemsCollected.Num(); ++j)
        {
            InventorySys->RemoveItem(Row->ItemsCollected[j]);
        }
    }

    // Phase 2: Award items from DependentAreas
    for (int32 i = 0; i < DependentAreas.Num(); ++i)
    {
        FDLCLevelSystemInfo* DepRow = LevelSystemData->FindRow<FDLCLevelSystemInfo>(
            DependentAreas[i], ContextString, false);
        if (!DepRow)
        {
            continue;
        }

        for (int32 j = 0; j < DepRow->ItemsCollected.Num(); ++j)
        {
            InventorySys->AwardItem(DepRow->ItemsCollected[j], 1);
        }
    }
}
