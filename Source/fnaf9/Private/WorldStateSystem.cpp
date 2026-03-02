#include "WorldStateSystem.h"
#include "FNAFBasePlayerCharacter.h"
#include "FNAFGameInstanceBase.h"
#include "FNAFSaveData.h"
#include "FNAFSaveGameSystem.h"
#include "GameClockSystem.h"
#include "AIManagementSystem.h"

UWorldStateSystem::UWorldStateSystem()
{
    WorldState = EFNAFGameState::Normal;
    FreddyState = FFreddyUpgradeState();
    SeededRandomStream = FRandomStream();
    AIState = FFNAFAISaveData();
    RuinAIState = FFNAFRuinAISaveData();

    bEnterExitFreddyEnabled = true;
    bCanCallFreddy = false;
    bPlayerInFreddy = false;
    bFreddySick = false;

    FreddyPawn = nullptr;
    FreddyPatrolPoint = 0;
    FreddyPatrolPathName = FName();

    bPlayerInPowerStation = false;
    PowerStationID = 0;

    bCanUsePowerStation = false;
    bPlayerUsedHideSpot = false;
    bCanShowInstructionCards = true;
    bInstructionCardShown = false;

    GoalPathName = FName();

    PlayerDeathCount = 0;
    PlayerSpottedCount = 0;
    SurvivalMaxDeaths = 0;

    LastSavedPlayerLocation = FVector::ZeroVector;
    LastSavedPlayerRotation = FRotator::ZeroRotator;
    FreddyWasInWorld = false;
    LastSavedFreddyLocation = FVector::ZeroVector;
    LastSavedFreddyRotation = FRotator::ZeroRotator;
}

void UWorldStateSystem::SetupNewGame()
{
    WorldState = EFNAFGameState::Normal;
    AIState = FFNAFAISaveData();
    RuinAIState = FFNAFRuinAISaveData();
    ArcadeState = FArcadeSaveData();
    FreddyState = FFreddyUpgradeState();
    ActivatedObjects.Empty();
    MazercisePanelLocations.Empty();
    SeededRandomStream.Reset();

    FreddyWasInWorld = false;
    bPlayerInPowerStation = false;
    bCanUsePowerStation = true;
    bPlayerUsedHideSpot = false;
    bPlayerInFreddy = false;
    PowerStationID = -1;
    PlayerDeathCount = 0;
    PlayerSpottedCount = 0;
}

void UWorldStateSystem::SetWorldState(EFNAFGameState NewState)
{
    EFNAFGameState OldState = WorldState;
    if (OldState != NewState)
    {
        WorldState = NewState;
        if (OnWorldStateChanged.IsBound())
        {
            OnWorldStateChanged.Broadcast(NewState, OldState);
        }
    }
}

EFNAFGameState UWorldStateSystem::GetWorldState() const
{
    return WorldState;
}

void UWorldStateSystem::StartMinigame(const FString& MinigameName, EFNAFGameState GameState, AActor* MinigameActor)
{
    CurrentMinigameName = MinigameName;
    EFNAFGameState OldState = WorldState;
    CurrentMinigameActor = MinigameActor;
    if (OldState != GameState)
    {
        WorldState = GameState;
        if (OnWorldStateChanged.IsBound())
        {
            OnWorldStateChanged.Broadcast(GameState, OldState);
        }
    }
}

void UWorldStateSystem::EndMinigame()
{
    CurrentMinigameName.Empty();
    EFNAFGameState OldState = WorldState;
    CurrentMinigameActor = nullptr;
    if (OldState != EFNAFGameState::Normal)
    {
        WorldState = EFNAFGameState::Normal;
        if (OnWorldStateChanged.IsBound())
        {
            OnWorldStateChanged.Broadcast(EFNAFGameState::Normal, OldState);
        }
    }
}

/* Checks survival mode, world state, and queries AIManagementSystem for
   enemies in the player's current rooms. CanStart is true even when
   PlayerInDanger -- the reason is informational only. */
void UWorldStateSystem::CanStartMinigame(bool& CanStart, ECantStartMinigameReason& reason) const
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFGameInstanceBase* FNAFInstance = Cast<UFNAFGameInstanceBase>(GameInstance);
        if (FNAFInstance && FNAFInstance->IsSurvivalMode())
        {
            reason = ECantStartMinigameReason::Survival;
            CanStart = false;
            return;
        }
    }

    if (WorldState == EFNAFGameState::MoonManDanger)
    {
        reason = ECantStartMinigameReason::MoonMan;
        CanStart = false;
    }
    else if (WorldState == EFNAFGameState::RepairGame || WorldState == EFNAFGameState::OfficeGame)
    {
        reason = ECantStartMinigameReason::AlreadyInMinigame;
        CanStart = false;
    }
    else
    {
        /* TODO: Full implementation queries AIManagementSystem + RoomSystem to check
           if any tracked AI overlaps the player's current rooms. */
        bool bEnemiesInRoom = false;

        UWorld* World = GetWorld();
        if (World)
        {
            UAIManagementSystem* AIMgmt = World->GetSubsystem<UAIManagementSystem>();
            if (AIMgmt)
            {
                    //bEnemiesInRoom = AIMgmt->GetActivePursuitCount() > 0;
            }
        }

        if (bEnemiesInRoom)
        {
            reason = ECantStartMinigameReason::PlayerInDanger;
        }
        else
        {
            reason = ECantStartMinigameReason::None;
        }
        CanStart = true;
    }
}

AActor* UWorldStateSystem::GetCurrentMinigameActor() const
{
    return CurrentMinigameActor;
}

FString UWorldStateSystem::GetCurrentMinigame() const
{
    return CurrentMinigameName;
}

void UWorldStateSystem::SetCanEnterExitFreddy(bool bCanEnterExit)
{
    bEnterExitFreddyEnabled = bCanEnterExit;
}

bool UWorldStateSystem::CanEnterExitFreddy() const
{
    return bEnterExitFreddyEnabled;
}

void UWorldStateSystem::SetCanCallFreddy(bool bCanCall)
{
    bCanCallFreddy = bCanCall;
}

bool UWorldStateSystem::CanCallFreddy() const
{
    return bCanCallFreddy;
}

void UWorldStateSystem::SetPlayerInFreddy(bool bInFreddy)
{
    bPlayerInFreddy = bInFreddy;
}

bool UWorldStateSystem::IsPlayerInFreddy() const
{
    return bPlayerInFreddy;
}

AFNAFBasePlayerCharacter* UWorldStateSystem::GetFreddyPawn() const
{
    return FreddyPawn;
}

void UWorldStateSystem::SetFreddySick(bool bIsSick)
{
    bFreddySick = bIsSick;
}

bool UWorldStateSystem::IsFreddySick() const
{
    return bFreddySick;
}

void UWorldStateSystem::SetFreddyPatrolPoint(int32 PatrolPointIndex)
{
    FreddyPatrolPoint = PatrolPointIndex;
}

int32 UWorldStateSystem::GetFreddyPatrolPoint() const
{
    return FreddyPatrolPoint;
}

void UWorldStateSystem::SetGoalPathName(FName inGoalPathName)
{
    GoalPathName = inGoalPathName;
}

FName UWorldStateSystem::GetGoalPathName()
{
    return GoalPathName;
}

void UWorldStateSystem::AddActivated(FName ActivatableName)
{
    if (ActivatableName != NAME_None)
    {
        if (!ActivatedObjects.Contains(ActivatableName))
        {
            ActivatedObjects.Add(ActivatableName);
            OnObjectStateChanged.Broadcast(ActivatableName, true);
        }
    }
}

void UWorldStateSystem::RemoveActivated(FName ActivatableName)
{
    int32 NumRemoved = ActivatedObjects.Remove(ActivatableName);
    if (NumRemoved > 0)
    {
        OnObjectStateChanged.Broadcast(ActivatableName, false);
    }
}

bool UWorldStateSystem::IsActivated(FName ActivatableName) const
{
    return ActivatedObjects.Contains(ActivatableName);
}

TArray<FName> UWorldStateSystem::GetAllCurrentActivables()
{
    return ActivatedObjects.Array();
}

void UWorldStateSystem::SetPowerStationAvailable(bool bAvailable)
{
    bCanUsePowerStation = bAvailable;
}

bool UWorldStateSystem::IsPowerStationAvailable() const
{
    return bCanUsePowerStation;
}

void UWorldStateSystem::SetPlayerInPowerStation(int32 InPowerStationID)
{
    int32 SaveIteration = 0;
    int32 Hour = -1;
    int32 Minute = 0;

    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFSaveGameSystem* SaveSystem = GameInstance->GetSubsystem<UFNAFSaveGameSystem>();
        if (SaveSystem)
        {
            //SaveIteration = SaveSystem->GetCurrentIteration();
        }

        UGameClockSystem* ClockSystem = GameInstance->GetSubsystem<UGameClockSystem>();
        if (ClockSystem)
        {
            float TotalSeconds = ClockSystem->GetCurrentTimeInSeconds();
            if (TotalSeconds >= 0.0f)
            {
                int32 TotalMinutes = (int32)TotalSeconds / 60;
                Hour = TotalMinutes / 60;
                Minute = TotalMinutes % 60;
            }
            else
            {
                Hour = -1;
                Minute = (int32)(TotalSeconds * 0.016666668f + 60.000004f);
            }
        }
    }

    FPowerStationSaveInfo Info;
    Info.PowerStationID = InPowerStationID;
    Info.GameIteration = SaveIteration;
    Info.GameHour = Hour;
    Info.GameMinute = Minute;
    PowerStationInfo.Add(Info);

    bPlayerInPowerStation = true;
    PowerStationID = InPowerStationID;

    EFNAFGameState OldState = WorldState;
    if (OldState != EFNAFGameState::PowerCycle)
    {
        WorldState = EFNAFGameState::PowerCycle;
        if (OnWorldStateChanged.IsBound())
        {
            OnWorldStateChanged.Broadcast(EFNAFGameState::PowerCycle, OldState);
        }
    }
}

void UWorldStateSystem::ClearPlayerInPowerStation()
{
    bPlayerInPowerStation = false;
}

void UWorldStateSystem::GetPowerStationInfo(bool& OutPlayerInPowerStation, int32& OutPowerStationID) const
{
    OutPlayerInPowerStation = bPlayerInPowerStation;
    OutPowerStationID = PowerStationID;
}

// Returns remaining lives in survival mode, or -1 for infinite lives
void UWorldStateSystem::AddDeath(int32& OutRemainingLives)
{
    ++PlayerDeathCount;

    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFGameInstanceBase* FNAFInstance = Cast<UFNAFGameInstanceBase>(GameInstance);
        if (FNAFInstance && FNAFInstance->IsSurvivalMode() && SurvivalMaxDeaths != -1)
        {
            OutRemainingLives = SurvivalMaxDeaths - PlayerDeathCount;
            return;
        }
    }
    OutRemainingLives = -1;
}

int32 UWorldStateSystem::GetLivesRemaining() const
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFGameInstanceBase* FNAFInstance = Cast<UFNAFGameInstanceBase>(GameInstance);
        if (FNAFInstance && FNAFInstance->IsSurvivalMode() && SurvivalMaxDeaths != -1)
        {
            return SurvivalMaxDeaths - PlayerDeathCount;
        }
    }
    return -1;
}

int32 UWorldStateSystem::GetCurrentDeathCount() const
{
    return PlayerDeathCount;
}

void UWorldStateSystem::AddSpotted()
{
    ++PlayerSpottedCount;
}

int32 UWorldStateSystem::GetCurrentSpottedCount() const
{
    return PlayerSpottedCount;
}

void UWorldStateSystem::SetSurvivalMaxDeaths(int32 MaxDeaths)
{
    SurvivalMaxDeaths = MaxDeaths;
}

int32 UWorldStateSystem::GetSurvivalMaxDeaths() const
{
    return SurvivalMaxDeaths;
}

void UWorldStateSystem::SetSurvivalDifficulty(ESurvivalDifficulty Difficulty)
{
    SurvivalDifficulty = Difficulty;
}

ESurvivalDifficulty UWorldStateSystem::GetSurvivalDifficulty() const
{
    return SurvivalDifficulty;
}

void UWorldStateSystem::SetCanShowInstructionCards(bool bInCanShow)
{
    bCanShowInstructionCards = bInCanShow;
}

bool UWorldStateSystem::CanShowInstructionCards() const
{
    return bCanShowInstructionCards;
}

void UWorldStateSystem::SetInstructionCardShown(bool Shown)
{
    bInstructionCardShown = Shown;
}

bool UWorldStateSystem::IsInstructionCardShown() const
{
    return bInstructionCardShown;
}

void UWorldStateSystem::SetPlayerHasUsedHidingSpot()
{
    bPlayerUsedHideSpot = true;
}

bool UWorldStateSystem::HasPlayerUsedHidingSpot() const
{
    return bPlayerUsedHideSpot;
}

void UWorldStateSystem::DebugSetPlayerHasUsedHidingSpot(bool HasHid)
{
    bPlayerUsedHideSpot = HasHid;
}

void UWorldStateSystem::SetRandomSeed(int32 Seed)
{
    SeededRandomStream.Initialize(Seed);
}

int32 UWorldStateSystem::GetRandomSeed() const
{
    return SeededRandomStream.GetInitialSeed();
}

void UWorldStateSystem::SetAIState(const FFNAFAISaveData& InAIState)
{
    AIState = InAIState;
}

FFNAFAISaveData UWorldStateSystem::GetAIState() const
{
    return AIState;
}

void UWorldStateSystem::SetRuinAIState(const FFNAFRuinAISaveData& InRuinAIState)
{
    RuinAIState = InRuinAIState;
}

FFNAFRuinAISaveData UWorldStateSystem::GetRuinAIState() const
{
    return RuinAIState;
}

void UWorldStateSystem::SetArcadeState(const FArcadeSaveData& InArcadeState)
{
    ArcadeState = InArcadeState;
}

FArcadeSaveData UWorldStateSystem::GetArcadeState() const
{
    return ArcadeState;
}

void UWorldStateSystem::SetMazercisePanel(int32 Index, const FString& Location)
{
    if (Index >= 0)
    {
        MazercisePanelLocations.Add(Index, Location);
    }
}

FString UWorldStateSystem::GetMazercisePanel(int32 Index)
{
    FString* Found = MazercisePanelLocations.Find(Index);
    if (Found)
    {
        return *Found;
    }
    return FString();
}

void UWorldStateSystem::SetDeactivatedCautionBot_Map(TMap<int32, bool> In_DeactivatedCautionBots_Map)
{
    DeactivatedCautionBots_Map = MoveTemp(In_DeactivatedCautionBots_Map);
}

TMap<int32, bool> UWorldStateSystem::GetDeactivatedCautionBots_Map()
{
    return DeactivatedCautionBots_Map;
}

void UWorldStateSystem::AddDeactivatedCautionBot(int32 CautionBotID, bool isDeactivated)
{
    if (CautionBotID != 0)
    {
        DeactivatedCautionBots_Map.Add(CautionBotID, isDeactivated);
    }
}

void UWorldStateSystem::RemoveDeactivatedCautionBot(int32 CautionBotID, bool isDeactivated)
{
    if (CautionBotID != 0)
    {
        DeactivatedCautionBots_Map.Remove(CautionBotID);
    }
}

void UWorldStateSystem::AddBonnieBowlMMMJumpscareSaveID(FName inSaveID)
{
    BonnieBowlMMMJumpscareData.Add(inSaveID);
}

void UWorldStateSystem::RemoveBonnieBowlMMMJumpscareSaveID(FName inSaveID)
{
    BonnieBowlMMMJumpscareData.Remove(inSaveID);
}

TSet<FName> UWorldStateSystem::GetBonnieBowlMMMJumspscareData()
{
    return BonnieBowlMMMJumpscareData;
}

void UWorldStateSystem::SetCurrentActivityId(const FString& InActivityId)
{
    CurrentActivityId = InActivityId;
}

FString UWorldStateSystem::GetCurrentActivityId() const
{
    return CurrentActivityId;
}

void UWorldStateSystem::GetSavedPlayerLocationAndRotation(FVector& OutWorldLocation, FRotator& OutWorldRotation) const
{
    OutWorldLocation = LastSavedPlayerLocation;
    OutWorldRotation = LastSavedPlayerRotation;
}

void UWorldStateSystem::GetSavedFreddyLocationAndRotation(bool& OutFreddyInWorld, FVector& OutWorldLocation, FRotator& OutWorldRotation) const
{
    OutFreddyInWorld = FreddyWasInWorld;
    OutWorldLocation = LastSavedFreddyLocation;
    OutWorldRotation = LastSavedFreddyRotation;
}

void UWorldStateSystem::ResetForChapterSelect(TArray<FName> chapterActivatables, TArray<FName> activatablesToKeepOnReplay, int32 chapterSelected)
{
    // Save current activated objects before reset
    LoadedActivatedObjects.Empty();
    LoadedActivatedObjects.Append(ActivatedObjects);

    ActivatedObjects.Empty();
    for (const FName& Name : chapterActivatables)
    {
        ActivatedObjects.Add(Name);
    }

    for (const FName& Name : activatablesToKeepOnReplay)
    {
        if (LoadedActivatedObjects.Contains(Name))
        {
            ActivatedObjects.Add(Name);
        }
    }
}

// ISaveHandlerInterface -- OnCheckpointSave/Load delegate to OnStoreGameData/OnGameDataLoaded
void UWorldStateSystem::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject) return;

    SaveDataObject->WorldStateData.ActivatedObjects = ActivatedObjects;
    SaveDataObject->WorldStateData.bFreddyInWorld = (FreddyPawn != nullptr) && !bPlayerInPowerStation;
    SaveDataObject->WorldStateData.bCanCallFreddy = bCanCallFreddy;
    SaveDataObject->WorldStateData.bCanEnterExitFreddy = bEnterExitFreddyEnabled;
    SaveDataObject->WorldStateData.bIsInFreddy = bPlayerInFreddy;
    SaveDataObject->WorldStateData.bUseSickFreddy = bFreddySick;
    SaveDataObject->WorldStateData.FreddyPatrolPoint = FreddyPatrolPoint;
    SaveDataObject->WorldStateData.GameState = WorldState;
    SaveDataObject->WorldStateData.bPlayerUsedHidingSpace = bPlayerUsedHideSpot;
    SaveDataObject->WorldStateData.bCanUsePowerStation = bCanUsePowerStation;

    SaveDataObject->WorldStateData.MazercisePanelLocations = MazercisePanelLocations;

    if (FreddyPawn)
    {
        USceneComponent* RootComp = FreddyPawn->GetRootComponent();
        if (RootComp)
        {
            SaveDataObject->WorldStateData.FreddyPosition = RootComp->GetComponentLocation();
            SaveDataObject->WorldStateData.FreddyRotation = RootComp->GetComponentRotation();
        }
    }

    SaveDataObject->AISaveData = AIState;
    SaveDataObject->ArcadeSaveData = ArcadeState;
    SaveDataObject->FreddyUpgrades = FreddyState;
    SaveDataObject->bInPowerStation = bPlayerInPowerStation;
    SaveDataObject->PowerStationID = PowerStationID;

    SaveDataObject->ActivityId = CurrentActivityId;

    // TODO: DLC subclass (UFNAFChowdaSaveData) stores additional data:
    // LoadedActivatedObjects, BonnieBowlMMMJumpscareData, DeactivatedCautionBots_Map
}

void UWorldStateSystem::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject) return;

    ActivatedObjects = SaveDataObject->WorldStateData.ActivatedObjects;
    MazercisePanelLocations = SaveDataObject->WorldStateData.MazercisePanelLocations;
    AIState = SaveDataObject->AISaveData;
    ArcadeState = SaveDataObject->ArcadeSaveData;
    FreddyState = SaveDataObject->FreddyUpgrades;
    LastSavedPlayerLocation = SaveDataObject->PlayerLocation;
    LastSavedPlayerRotation = SaveDataObject->PlayerRotation;

    bPlayerUsedHideSpot = SaveDataObject->WorldStateData.bPlayerUsedHidingSpace;
    FreddyWasInWorld = SaveDataObject->WorldStateData.bFreddyInWorld && !SaveDataObject->bInPowerStation;

    LastSavedFreddyLocation = SaveDataObject->WorldStateData.FreddyPosition;
    LastSavedFreddyRotation = SaveDataObject->WorldStateData.FreddyRotation;

    bCanCallFreddy = SaveDataObject->WorldStateData.bCanCallFreddy;
    bEnterExitFreddyEnabled = SaveDataObject->WorldStateData.bCanEnterExitFreddy;
    bPlayerInFreddy = SaveDataObject->WorldStateData.bIsInFreddy;
    bFreddySick = SaveDataObject->WorldStateData.bUseSickFreddy;
    FreddyPatrolPoint = SaveDataObject->WorldStateData.FreddyPatrolPoint;
    WorldState = SaveDataObject->WorldStateData.GameState;
    bPlayerInPowerStation = SaveDataObject->bInPowerStation;
    PowerStationID = SaveDataObject->PowerStationID;
    bCanUsePowerStation = SaveDataObject->WorldStateData.bCanUsePowerStation;
    CurrentActivityId = SaveDataObject->ActivityId;

    // TODO: DLC subclass (UFNAFChowdaSaveData) restores additional data
}

void UWorldStateSystem::OnCheckpointSave_Implementation(UFNAFSaveData* SaveDataObject)
{
    OnStoreGameData_Implementation(SaveDataObject);
}

void UWorldStateSystem::OnCheckpointLoad_Implementation(UFNAFSaveData* SaveDataObject)
{
    OnGameDataLoaded_Implementation(SaveDataObject);
}

void UWorldStateSystem::PostSaveGame_Implementation()
{
}

void UWorldStateSystem::PostGameLoad_Implementation()
{
}