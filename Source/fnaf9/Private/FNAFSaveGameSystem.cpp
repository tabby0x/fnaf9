#include "FNAFSaveGameSystem.h"
#include "FNAFSaveData.h"
#include "FNAFChowdaSaveData.h"
#include "FNAFMasterData.h"
#include "FNAFGameInstanceBase.h"
#include "SaveHandlerInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"

// Static member definitions
FString UFNAFSaveGameSystem::CHOWDA_AUTOSAVE_SLOT_NAME = TEXT("ChowdaAutoSave");
FString UFNAFSaveGameSystem::AUTOSAVE_SLOT_NAME = TEXT("AutoSave");
FString UFNAFSaveGameSystem::WORLD_TRANSIT_SLOT_NAME = TEXT("WorldTransit");

UFNAFSaveGameSystem::UFNAFSaveGameSystem()
{
    MaxNumOfSaveSlots = 50;
    LoadedGameTime = 0.0f;
    SaveSlotsInitalized = false;
    bChowdaSaveSlotsInitialized = false;

    MasterDataObject = Cast<UFNAFMasterData>(
        UGameplayStatics::CreateSaveGameObject(UFNAFMasterData::StaticClass()));

    MasterDataSlotName = TEXT("MasterDataSlot");

    bIsSavingAllowed = true;
    bInvertedGamepad = MasterDataObject ? MasterDataObject->InvertedGamepad : false;
    UserIndex = 0;
    ChowdaProfileIndex = 0;

    SaveSlotName = TEXT("SaveGameSlot") + FString::FromInt(0);

    if (SaveSlotName.Len() > 0)
    {
        if (!HasSaveSlotMapping.Contains(SaveSlotName))
        {
            bool bExists = UGameplayStatics::DoesSaveGameExist(SaveSlotName, UserIndex);
            HasSaveSlotMapping.Add(SaveSlotName, bExists);
        }
        bDoesSaveGameExist = HasSaveSlotMapping.FindRef(SaveSlotName);
    }
    else
    {
        bDoesSaveGameExist = false;
    }

    if (MasterDataObject)
    {
        MasterDataObject->lastSavedSlotName = SaveSlotName;
        MasterDataObject->newSaveSlotNumber = 0;
    }

    MaxChowdaSaveSlots = 3;

    if (MasterDataObject)
    {
        MasterDataObject->lastSavedChowdaSlotName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ChowdaProfileIndex);
        MasterDataObject->lastLoadedChowdaSlotName = MasterDataObject->lastSavedChowdaSlotName;
    }

    FMemory::Memzero(TakenSlots, sizeof(TakenSlots));

    bInChapterReplay = false;
    ReplayCurrentChapter = 0;
    MapAreaOnLastAutosave = EMapArea::Lobby;
    UseAutosaveMapArea = false;
    LoadIntoChapter = false;
}

/* Loads persisted MasterDataObject from disk to restore save slot tracking. */
void UFNAFSaveGameSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    if (GetHasSaveFromMapping(MasterDataSlotName))
    {
        UFNAFMasterData* LoadedMaster = Cast<UFNAFMasterData>(
            UGameplayStatics::LoadGameFromSlot(MasterDataSlotName, UserIndex));
        if (LoadedMaster)
        {
            MasterDataObject = LoadedMaster;
        }
    }
}

/* Iterates all actors and subsystems implementing ISaveHandlerInterface, calls OnStoreGameData on each. */
void UFNAFSaveGameSystem::CollectSaveData(UFNAFSaveData* SaveData)
{
    if (!SaveData)
    {
        return;
    }

    SaveData->RealtimeSaveTime = FDateTime::Now();

    float CurrentGameTime = UKismetSystemLibrary::GetGameTimeInSeconds(this);
    float DeltaTime = CurrentGameTime - LoadedGameTime;
    LoadedGameTime = CurrentGameTime;
    SaveData->TotalTimePlayedInSeconds = (int32)((float)SaveData->TotalTimePlayedInSeconds + DeltaTime);

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveData);
    if (ChowdaSave)
    {
        ChowdaSave->bInChapterReplay = bInChapterReplay;
    }

    SaveActorClasses.Reset();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    if (OnSaveGameBegin.IsBound())
    {
        OnSaveGameBegin.Broadcast();
    }

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            SaveActorClasses.Add(Actor);
            ISaveHandlerInterface::Execute_OnStoreGameData(Actor, SaveData);
        }
    }

    UGameInstance* GI = World->GetGameInstance();
    if (GI)
    {
        GameInstanceSubsystemSaveArray.Reset();
        const TArray<UGameInstanceSubsystem*>& GISubsystems = GI->GetSubsystemArray<UGameInstanceSubsystem>();
        for (UGameInstanceSubsystem* Subsystem : GISubsystems)
        {
            GameInstanceSubsystemSaveArray.Add(Subsystem);
            if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
            {
                ISaveHandlerInterface::Execute_OnStoreGameData(Subsystem, SaveData);
            }
        }
    }

    WorldSubsystemSaveArray.Reset();
    const TArray<UWorldSubsystem*>& WorldSubsystems = World->GetSubsystemArray<UWorldSubsystem>();
    for (UWorldSubsystem* Subsystem : WorldSubsystems)
    {
        WorldSubsystemSaveArray.Add(Subsystem);
        if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            ISaveHandlerInterface::Execute_OnStoreGameData(Subsystem, SaveData);
        }
    }
}

bool UFNAFSaveGameSystem::GetHasSaveFromMapping(const FString& SlotName)
{
    if (SlotName.Len() <= 0)
    {
        return false;
    }

    if (!HasSaveSlotMapping.Contains(SlotName))
    {
        bool bExists = UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex);
        HasSaveSlotMapping.Add(SlotName, bExists);
    }

    return HasSaveSlotMapping.FindRef(SlotName);
}

void UFNAFSaveGameSystem::UpdateHasSaveMapping(const FString& SlotName, bool HasSave)
{
    if (SlotName.Len() <= 0)
    {
        return;
    }

    if (HasSaveSlotMapping.Contains(SlotName))
    {
        HasSaveSlotMapping[SlotName] = HasSave;
    }
    else
    {
        HasSaveSlotMapping.Add(SlotName, HasSave);
    }
}

/* Extracts the numeric index from a slot name like "SaveGameSlot5" -> 5 */
int32 UFNAFSaveGameSystem::GetSaveSlotNum(const FString& SlotName)
{
    FString Prefix = TEXT("SaveGameSlot");
    FString NumStr = SlotName.Replace(*Prefix, TEXT(""));
    return FCString::Atoi(*NumStr);
}

int32 UFNAFSaveGameSystem::GetNextAvailableSaveSlotIndex()
{
    // First time: initialize TakenSlots from MasterDataObject's SaveGameSlotNames_Map
    if (!SaveSlotsInitalized)
    {
        if (MasterDataObject)
        {
            for (auto& Pair : MasterDataObject->SaveGameSlotNames_Map)
            {
                int32 SlotNum = GetSaveSlotNum(Pair.Key);
                if (SlotNum >= 0 && SlotNum < MaxNumOfSaveSlots)
                {
                    TakenSlots[SlotNum] = true;
                }
            }
        }
        SaveSlotsInitalized = true;
    }

    for (int32 i = 0; i < MaxNumOfSaveSlots; ++i)
    {
        if (!TakenSlots[i])
        {
            return i;
        }
    }

    return -1;
}

void UFNAFSaveGameSystem::OnGameSaveCompleteHandler(const FString& SlotName, const int32 PlayerIndex, bool bSuccess)
{
    UpdateHasSaveMapping(SlotName, true);
    bIsSavingAllowed = true;

    if (OnSaveGameComplete.IsBound())
    {
        OnSaveGameComplete.Broadcast();
    }
}

/* Chains to save the actual chowda data after master save completes. */
void UFNAFSaveGameSystem::OnChowdaMasterSaveCompleteHandler(const FString& SlotName, const int32 PlayerIndex, bool bSuccess)
{
    FString SaveName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ChowdaProfileIndex);

    FAsyncSaveGameToSlotDelegate SaveDelegate;
    SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
    UGameplayStatics::AsyncSaveGameToSlot(SaveDataObject, SaveName, UserIndex, SaveDelegate);

    UpdateHasSaveMapping(SaveName, true);
}

void UFNAFSaveGameSystem::OnGameLoadCompleteHandler(const FString& SlotName, const int32 PlayerIndex, USaveGame* LoadedSaveGame)
{
    UFNAFSaveData* LoadedData = Cast<UFNAFSaveData>(LoadedSaveGame);
    SaveDataObject = LoadedData;

    if (LoadedData)
    {
        LoadedGameTime = UKismetSystemLibrary::GetGameTimeInSeconds(this);

        UWorld* World = GetWorld();
        if (World)
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (Actor && Actor->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
                {
                    ISaveHandlerInterface::Execute_OnGameDataLoaded(Actor, LoadedData);
                }
            }

            UGameInstance* GI = World->GetGameInstance();
            if (GI)
            {
                const TArray<UGameInstanceSubsystem*>& GISubsystems = GI->GetSubsystemArray<UGameInstanceSubsystem>();
                for (UGameInstanceSubsystem* Subsystem : GISubsystems)
                {
                    if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
                    {
                        ISaveHandlerInterface::Execute_OnGameDataLoaded(Subsystem, LoadedData);
                    }
                }
            }

            const TArray<UWorldSubsystem*>& WorldSubsystems = World->GetSubsystemArray<UWorldSubsystem>();
            for (UWorldSubsystem* Subsystem : WorldSubsystems)
            {
                if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
                {
                    ISaveHandlerInterface::Execute_OnGameDataLoaded(Subsystem, LoadedData);
                }
            }

            for (TActorIterator<AActor> It2(World); It2; ++It2)
            {
                AActor* Actor = *It2;
                if (Actor && Actor->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
                {
                    ISaveHandlerInterface::Execute_PostGameLoad(Actor);
                }
            }
        }
    }

    if (OnLoadGameComplete.IsBound())
    {
        OnLoadGameComplete.Broadcast();
    }
}

void UFNAFSaveGameSystem::LoadGameInternal(const FString& InSaveSlotName)
{
    bDoesSaveGameExist = GetHasSaveFromMapping(InSaveSlotName);

    if (bDoesSaveGameExist)
    {
        FAsyncLoadGameFromSlotDelegate LoadDelegate;
        LoadDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameLoadCompleteHandler);
        UGameplayStatics::AsyncLoadGameFromSlot(InSaveSlotName, UserIndex, LoadDelegate);
    }
}

void UFNAFSaveGameSystem::AsyncSaveGame(const FString& originalSlotName, bool isANewSlot, bool isCurrentSlot, const FString& Renamed_SaveSlot, FString& internal_SlotName)
{
    if (!bIsSavingAllowed)
    {
        return;
    }

    if (!SaveDataObject)
    {
        SaveDataObject = Cast<UFNAFSaveData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));
    }

    if (isANewSlot)
    {
        int32 NextIndex = GetNextAvailableSaveSlotIndex();
        if (MasterDataObject)
        {
            MasterDataObject->newSaveSlotNumber = NextIndex;
        }

        SaveSlotName = TEXT("SaveGameSlot") + FString::FromInt(NextIndex);

        if (MasterDataObject)
        {
            MasterDataObject->SaveGameSlotNames_Map.Add(SaveSlotName, Renamed_SaveSlot);
        }

        int32 SlotNum = GetSaveSlotNum(SaveSlotName);
        if (SlotNum >= 0 && SlotNum < MaxNumOfSaveSlots)
        {
            TakenSlots[SlotNum] = true;
        }

        if (MasterDataObject)
        {
            MasterDataObject->lastSavedSlotName = TEXT("SaveGameSlot") + FString::FromInt(NextIndex);
        }
    }
    else if (!isCurrentSlot)
    {
        SaveSlotName = originalSlotName;
    }

    if (MasterDataObject)
    {
        MasterDataObject->bLastSaveWasAuto = false;
        MasterDataObject->lastSavedSlotName = SaveSlotName;
        MasterDataObject->lastLoadedSlotName = SaveSlotName;
        MasterDataObject->ActivitySaveSlot = SaveSlotName;
    }

    if (SaveDataObject && SaveDataObject->ActivityId.Len() > 0 && MasterDataObject)
    {
        MasterDataObject->ActivityIdSaveSlotNamesMap.Add(SaveDataObject->ActivityId, SaveSlotName);
    }

    internal_SlotName = SaveSlotName;

    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);

    if (SaveDataObject && bIsSavingAllowed)
    {
        bIsSavingAllowed = false;
        CollectSaveData(SaveDataObject);

        FAsyncSaveGameToSlotDelegate SaveDelegate;
        SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
        UGameplayStatics::AsyncSaveGameToSlot(SaveDataObject, SaveSlotName, UserIndex, SaveDelegate);
    }
}

void UFNAFSaveGameSystem::AutoSave()
{
    if (!bIsSavingAllowed)
    {
        return;
    }

    if (!SaveDataObject)
    {
        SaveDataObject = Cast<UFNAFSaveData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));
    }

    CollectSaveData(SaveDataObject);

    if (MasterDataObject)
    {
        MasterDataObject->bLastSaveWasAuto = true;
        MasterDataObject->lastLoadedSlotName = AUTOSAVE_SLOT_NAME;
        MasterDataObject->ActivitySaveSlot = AUTOSAVE_SLOT_NAME;
    }

    if (SaveDataObject && SaveDataObject->ActivityId.Len() > 0 && MasterDataObject)
    {
        MasterDataObject->ActivityIdSaveSlotNamesMap.Add(SaveDataObject->ActivityId, AUTOSAVE_SLOT_NAME);
    }

    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);

    FAsyncSaveGameToSlotDelegate SaveDelegate;
    SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
    UGameplayStatics::AsyncSaveGameToSlot(SaveDataObject, AUTOSAVE_SLOT_NAME, UserIndex, SaveDelegate);

    UpdateHasSaveMapping(AUTOSAVE_SLOT_NAME, true);
}

void UFNAFSaveGameSystem::ChowdaAutoSave(int32 Chapter, EMapArea MapAreaToSave, FName SaveID)
{
    ShowAutoSaveIcon();

    UGameInstance* GI = GetGameInstance();
    UFNAFGameInstanceBase* FNAFGI = Cast<UFNAFGameInstanceBase>(GI);
    if (!FNAFGI || !bIsSavingAllowed)
    {
        return;
    }

    bIsSavingAllowed = false;

    if (!SaveDataObject)
    {
        SaveDataObject = Cast<UFNAFSaveData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFChowdaSaveData::StaticClass()));
    }

    FString SaveName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ChowdaProfileIndex);

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        ChowdaSave->bPlayerInNormal = false;

        if (bInChapterReplay)
        {
            // In chapter replay: only save if we're past the current chapter
            if (Chapter <= ChowdaSave->Chapter
                && SaveID != ChowdaSave->LastAutoSaveID
                && (!ChowdaSave->FirstSaveInChapter || Chapter != ChowdaSave->Chapter))
            {
                ReplayCurrentChapter = Chapter;
                MapAreaOnLastAutosave = MapAreaToSave;
                UseAutosaveMapArea = true;
                goto DoCollectAndSave;
            }
            bInChapterReplay = false;
        }

        ChowdaSave->FirstSaveInChapter = (ChowdaSave->Chapter < Chapter);
        ChowdaSave->LastAutoSaveID = SaveID;
        ChowdaSave->Chapter = Chapter;
        ChowdaSave->LastLoadedMapArea = MapAreaToSave;
    }

DoCollectAndSave:
    CollectSaveData(SaveDataObject);

    if (MasterDataObject)
    {
        MasterDataObject->lastSavedChowdaSlotName = SaveName;
        MasterDataObject->lastLoadedChowdaSlotName = SaveName;
    }
    SaveSlotName = SaveName;

    FAsyncSaveGameToSlotDelegate MasterSaveDelegate;
    MasterSaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnChowdaMasterSaveCompleteHandler);
    UGameplayStatics::AsyncSaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex, MasterSaveDelegate);
}

void UFNAFSaveGameSystem::SaveCurrentGame()
{
    if (!SaveDataObject || !bIsSavingAllowed)
    {
        return;
    }

    bIsSavingAllowed = false;
    CollectSaveData(SaveDataObject);

    FAsyncSaveGameToSlotDelegate SaveDelegate;
    SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
    UGameplayStatics::AsyncSaveGameToSlot(SaveDataObject, SaveSlotName, UserIndex, SaveDelegate);
}

void UFNAFSaveGameSystem::CreateWorldTransitSave()
{
    if (!bIsSavingAllowed)
    {
        return;
    }

    if (!WorldTransitDataObject)
    {
        UGameInstance* GI = GetGameInstance();
        UFNAFGameInstanceBase* FNAFGI = Cast<UFNAFGameInstanceBase>(GI);

        if (FNAFGI && FNAFGI->GetCurrentGameType() == EFNAFGameType::ChowdaMode)
        {
            WorldTransitDataObject = Cast<UFNAFSaveData>(
                UGameplayStatics::CreateSaveGameObject(UFNAFChowdaSaveData::StaticClass()));
        }
        else
        {
            WorldTransitDataObject = Cast<UFNAFSaveData>(
                UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));
        }
    }

    if (WorldTransitDataObject && bIsSavingAllowed)
    {
        bIsSavingAllowed = false;
        CollectSaveData(WorldTransitDataObject);

        FAsyncSaveGameToSlotDelegate SaveDelegate;
        SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
        UGameplayStatics::AsyncSaveGameToSlot(WorldTransitDataObject, WORLD_TRANSIT_SLOT_NAME, UserIndex, SaveDelegate);
    }
}

void UFNAFSaveGameSystem::CreatePotentialCheckpoint()
{
    UGameInstance* GI = GetGameInstance();
    UFNAFGameInstanceBase* FNAFGI = Cast<UFNAFGameInstanceBase>(GI);
    if (!FNAFGI || FNAFGI->GetCurrentGameType() == EFNAFGameType::ChowdaMode)
    {
        return;
    }

    TempSaveDataObject = Cast<UFNAFSaveData>(
        UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));

    SaveActorClasses.Reset();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    GISCheckpointArray.Reset();
    const TArray<UGameInstanceSubsystem*>& GISubsystems = World->GetGameInstance()->GetSubsystemArray<UGameInstanceSubsystem>();
    for (UGameInstanceSubsystem* Subsystem : GISubsystems)
    {
        GISCheckpointArray.Add(Subsystem);
    }

    WSCheckpointArray.Reset();
    const TArray<UWorldSubsystem*>& WorldSubsystems = World->GetSubsystemArray<UWorldSubsystem>();
    for (UWorldSubsystem* Subsystem : WorldSubsystems)
    {
        WSCheckpointArray.Add(Subsystem);
    }

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            SaveActorClasses.Add(Actor);
        }
    }

    for (AActor* Actor : SaveActorClasses)
    {
        if (Actor)
        {
            ISaveHandlerInterface::Execute_OnCheckpointSave(Actor, TempSaveDataObject);
        }
    }

    for (UObject* Subsystem : GISCheckpointArray)
    {
        if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            ISaveHandlerInterface::Execute_OnCheckpointSave(Subsystem, TempSaveDataObject);
        }
    }

    for (UObject* Subsystem : WSCheckpointArray)
    {
        if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            ISaveHandlerInterface::Execute_OnCheckpointSave(Subsystem, TempSaveDataObject);
        }
    }
}

void UFNAFSaveGameSystem::LoadCheckpoint()
{
    if (!SaveDataObject)
    {
        SaveDataObject = Cast<UFNAFSaveData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));
    }

    SaveActorClasses.Reset();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UGameInstance* GI = World->GetGameInstance();
    if (GI)
    {
        GameInstanceSubsystemSaveArray.Reset();
        const TArray<UGameInstanceSubsystem*>& GISubsystems = GI->GetSubsystemArray<UGameInstanceSubsystem>();
        for (UGameInstanceSubsystem* Subsystem : GISubsystems)
        {
            GameInstanceSubsystemSaveArray.Add(Subsystem);
        }
    }

    WorldSubsystemSaveArray.Reset();
    const TArray<UWorldSubsystem*>& WorldSubsystems = World->GetSubsystemArray<UWorldSubsystem>();
    for (UWorldSubsystem* Subsystem : WorldSubsystems)
    {
        WorldSubsystemSaveArray.Add(Subsystem);
    }

    for (UObject* Subsystem : GameInstanceSubsystemSaveArray)
    {
        if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            ISaveHandlerInterface::Execute_OnCheckpointLoad(Subsystem, SaveDataObject);
        }
    }

    for (UObject* Subsystem : WorldSubsystemSaveArray)
    {
        if (Subsystem && Subsystem->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            ISaveHandlerInterface::Execute_OnCheckpointLoad(Subsystem, SaveDataObject);
        }
    }

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor->GetClass()->ImplementsInterface(USaveHandlerInterface::StaticClass()))
        {
            SaveActorClasses.Add(Actor);
        }
    }

    for (AActor* Actor : SaveActorClasses)
    {
        if (Actor)
        {
            ISaveHandlerInterface::Execute_OnCheckpointLoad(Actor, SaveDataObject);
        }
    }
}

void UFNAFSaveGameSystem::AsyncLoadGame(const FString& originalSlotName, bool isContinueSelected, FString& internal_SlotName)
{
    if (isContinueSelected && MasterDataObject)
    {
        SaveSlotName = MasterDataObject->lastSavedSlotName;
    }
    else
    {
        SaveSlotName = originalSlotName;
    }

    LoadGameInternal(SaveSlotName);
    internal_SlotName = SaveSlotName;
}

void UFNAFSaveGameSystem::AsyncLoadGameLastLoaded()
{
    FString LastSave = AUTOSAVE_SLOT_NAME;

    if (MasterDataObject && MasterDataObject->lastLoadedSlotName.Len() > 0)
    {
        LastSave = MasterDataObject->lastLoadedSlotName;
    }

    LoadGameInternal(LastSave);
}

void UFNAFSaveGameSystem::AsyncLoadChowdaLastLoaded()
{
    if (MasterDataObject && MasterDataObject->lastLoadedChowdaSlotName.Len() > 0)
    {
        LoadGameInternal(MasterDataObject->lastLoadedChowdaSlotName);
    }
}

void UFNAFSaveGameSystem::LoadAutoSave()
{
    if (MasterDataObject)
    {
        LoadGameInternal(AUTOSAVE_SLOT_NAME);
    }
}

void UFNAFSaveGameSystem::LoadWorldTransitSave()
{
    LoadGameInternal(WORLD_TRANSIT_SLOT_NAME);
}

void UFNAFSaveGameSystem::LoadChowdaAutoSave(int32 ProfileIndexIn)
{
    ChowdaProfileIndex = ProfileIndexIn;

    FString SlotName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ProfileIndexIn);
    SaveSlotName = SlotName;

    LoadGameInternal(SlotName);
}

UFNAFSaveData* UFNAFSaveGameSystem::LoadChowdaAutoSaveData(int32 ProfileIndexIn)
{
    ChowdaProfileIndex = ProfileIndexIn;

    FString SlotName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ProfileIndexIn);

    USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex);
    return Cast<UFNAFSaveData>(Loaded);
}

UFNAFSaveData* UFNAFSaveGameSystem::LoadSaveSlotData(const FString& SlotName)
{
    USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex);
    return Cast<UFNAFSaveData>(Loaded);
}

void UFNAFSaveGameSystem::LoadActivitySave(const FString& InActivityId)
{
    if (!MasterDataObject)
    {
        return;
    }

    FString* FoundSlot = MasterDataObject->ActivityIdSaveSlotNamesMap.Find(InActivityId);
    if (FoundSlot)
    {
        if (GetHasSaveFromMapping(*FoundSlot))
        {
            LoadGameInternal(*FoundSlot);
        }
    }
}

void UFNAFSaveGameSystem::LoadMasterSave()
{
    if (GetHasSaveFromMapping(MasterDataSlotName))
    {
        UFNAFMasterData* LoadedMaster = Cast<UFNAFMasterData>(
            UGameplayStatics::LoadGameFromSlot(MasterDataSlotName, UserIndex));
        if (LoadedMaster)
        {
            MasterDataObject = LoadedMaster;
        }
    }
}

void UFNAFSaveGameSystem::SetupNewGame()
{
    SaveDataObject = Cast<UFNAFSaveData>(
        UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));

    int32 NextIndex = GetNextAvailableSaveSlotIndex();

    if (MasterDataObject)
    {
        MasterDataObject->newSaveSlotNumber = NextIndex;
    }

    FString NewSlotName = TEXT("SaveGameSlot") + FString::FromInt(NextIndex);

    if (MasterDataObject)
    {
        MasterDataObject->lastLoadedSlotName = NewSlotName;
        MasterDataObject->lastSavedSlotName = MasterDataObject->lastLoadedSlotName;
    }
    SaveSlotName = MasterDataObject ? MasterDataObject->lastSavedSlotName : NewSlotName;

    if (MasterDataObject)
    {
        MasterDataObject->SaveGameSlotNames_Map.Add(SaveSlotName, SaveSlotName);
    }

    int32 SlotNum = GetSaveSlotNum(SaveSlotName);
    if (SlotNum >= 0 && SlotNum < MaxNumOfSaveSlots)
    {
        TakenSlots[SlotNum] = true;
    }

    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);

    bIsSavingAllowed = false;
    FAsyncSaveGameToSlotDelegate SaveDelegate;
    SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
    UGameplayStatics::AsyncSaveGameToSlot(SaveDataObject, SaveSlotName, UserIndex, SaveDelegate);

    UpdateHasSaveMapping(SaveSlotName, true);
}

void UFNAFSaveGameSystem::SetupNewGameOnSlot(const FString& originalSlotName)
{
    SaveDataObject = Cast<UFNAFSaveData>(
        UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));

    if (MasterDataObject)
    {
        MasterDataObject->lastSavedSlotName = originalSlotName;
    }
    SaveSlotName = MasterDataObject ? MasterDataObject->lastSavedSlotName : originalSlotName;

    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);

    bIsSavingAllowed = false;
    FAsyncSaveGameToSlotDelegate SaveDelegate;
    SaveDelegate.BindUObject(this, &UFNAFSaveGameSystem::OnGameSaveCompleteHandler);
    UGameplayStatics::AsyncSaveGameToSlot(SaveDataObject, SaveSlotName, UserIndex, SaveDelegate);

    UpdateHasSaveMapping(SaveSlotName, true);
}

void UFNAFSaveGameSystem::SetupNewChowdaGame(int32 ProfileIndexIn)
{
    FString SlotName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ProfileIndexIn);

    SaveDataObject = Cast<UFNAFSaveData>(
        UGameplayStatics::CreateSaveGameObject(UFNAFChowdaSaveData::StaticClass()));

    if (MasterDataObject)
    {
        MasterDataObject->ChowdaSaveGameSlotNames_Map.Add(SlotName, CHOWDA_AUTOSAVE_SLOT_NAME);
    }

    UpdateHasSaveMapping(SlotName, true);
}

void UFNAFSaveGameSystem::SetupPIE()
{
    SaveDataObject = Cast<UFNAFSaveData>(
        UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));
}

void UFNAFSaveGameSystem::DeleteSaveGameByName(const FString& SlotName)
{
    if (GetHasSaveFromMapping(SlotName))
    {
        UGameplayStatics::DeleteGameInSlot(SlotName, UserIndex);
    }

    UpdateHasSaveMapping(SlotName, false);

    if (MasterDataObject)
    {
        MasterDataObject->SaveGameSlotNames_Map.Remove(SlotName);
    }

    int32 SlotNum = GetSaveSlotNum(SlotName);
    if (SlotNum >= 0 && SlotNum < MaxNumOfSaveSlots)
    {
        TakenSlots[SlotNum] = false;
    }

    SaveSlotName.Empty();
    SaveDataObject = nullptr;
    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);
}

void UFNAFSaveGameSystem::DeleteSavedGame_BySlot(int32 saveSlotNumber)
{
    SaveSlotName = TEXT("SaveGameSlot") + FString::FromInt(saveSlotNumber);

    if (GetHasSaveFromMapping(SaveSlotName))
    {
        UGameplayStatics::DeleteGameInSlot(SaveSlotName, UserIndex);
    }

    UpdateHasSaveMapping(SaveSlotName, false);

    if (MasterDataObject)
    {
        MasterDataObject->SaveGameSlotNames_Map.Remove(SaveSlotName);
    }

    int32 SlotNum = GetSaveSlotNum(SaveSlotName);
    if (SlotNum >= 0 && SlotNum < MaxNumOfSaveSlots)
    {
        TakenSlots[SlotNum] = false;
    }

    SaveSlotName.Empty();
    SaveDataObject = nullptr;
    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);
}

void UFNAFSaveGameSystem::DeleteChowdaSaveGameByName(const FString& SlotName)
{
    if (GetHasSaveFromMapping(SlotName))
    {
        UGameplayStatics::DeleteGameInSlot(SlotName, UserIndex);
    }

    UpdateHasSaveMapping(SlotName, false);

    if (MasterDataObject)
    {
        MasterDataObject->ChowdaSaveGameSlotNames_Map.Remove(SlotName);
    }
}

void UFNAFSaveGameSystem::ClearChowdaAutoSaves()
{
    for (int32 i = 1; i <= MaxChowdaSaveSlots; ++i)
    {
        FString SlotName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(i);

        bool bExists = GetHasSaveFromMapping(SlotName);
        if (bExists)
        {
            UGameplayStatics::DeleteGameInSlot(SlotName, UserIndex);

            if (MasterDataObject)
            {
                MasterDataObject->ChowdaSaveGameSlotNames_Map.Remove(SlotName);
            }

            UpdateHasSaveMapping(SlotName, false);

            UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);
        }
    }
}

void UFNAFSaveGameSystem::Rename_SaveSlot(const FString& originalSlotName, const FString& renamedSlotName)
{
    if (MasterDataObject)
    {
        MasterDataObject->SaveGameSlotNames_Map.Add(originalSlotName, renamedSlotName);
    }

    int32 SlotNum = GetSaveSlotNum(originalSlotName);
    if (SlotNum >= 0 && SlotNum < MaxNumOfSaveSlots)
    {
        TakenSlots[SlotNum] = true;
    }
}

void UFNAFSaveGameSystem::Reset_SaveSlots()
{
    if (MasterDataObject)
    {
        MasterDataObject->lastSavedSlotName = TEXT("SaveGameSlot") + FString::FromInt(0);
        MasterDataObject->newSaveSlotNumber = 0;
    }

    UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);
}

void UFNAFSaveGameSystem::SaveCheckpoint()
{
    CreatePotentialCheckpoint();
    SaveDataObject = TempSaveDataObject;
}

void UFNAFSaveGameSystem::FinalizeCheckpoint()
{
    SaveDataObject = TempSaveDataObject;
}

void UFNAFSaveGameSystem::SaveArcade()
{
    if (!bIsSavingAllowed)
    {
        return;
    }

    UFNAFSaveData* ArcadeSaveObj = nullptr;
    bool bIsChowda = false;

    if (WorldTransitDataObject)
    {
        bIsChowda = (Cast<UFNAFChowdaSaveData>(WorldTransitDataObject) != nullptr);
    }

    if (bIsChowda)
    {
        ArcadeSaveObj = Cast<UFNAFSaveData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFChowdaSaveData::StaticClass()));
    }
    else
    {
        ArcadeSaveObj = Cast<UFNAFSaveData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFSaveData::StaticClass()));
    }

    bIsSavingAllowed = false;
    CollectSaveData(ArcadeSaveObj);
    bIsSavingAllowed = true;

    if (WorldTransitDataObject && ArcadeSaveObj)
    {
        WorldTransitDataObject->ArcadeSaveData = ArcadeSaveObj->ArcadeSaveData;
        UGameplayStatics::SaveGameToSlot(WorldTransitDataObject, WORLD_TRANSIT_SLOT_NAME, UserIndex);
    }

    if (SaveDataObject && ArcadeSaveObj)
    {
        SaveDataObject->ArcadeSaveData.BalloonWorld.HighScore = ArcadeSaveObj->ArcadeSaveData.BalloonWorld.HighScore;
        SaveDataObject->ArcadeSaveData.ChicaFeedingFrenzy.HighScore = ArcadeSaveObj->ArcadeSaveData.ChicaFeedingFrenzy.HighScore;
        UGameplayStatics::SaveGameToSlot(SaveDataObject, SaveSlotName, UserIndex);
    }
}

void UFNAFSaveGameSystem::ShowAutoSaveIcon()
{
    AutoSaveTriggered.Broadcast();

    if (!AutoSaveIconWidgetClass.IsValid())
    {
        UClass* WidgetClass = StaticLoadClass(UUserWidget::StaticClass(), nullptr,
            TEXT("/Game/UI_DLC/WI_Autosave_Icon.WI_Autosave_Icon_C"));
        AutoSaveIconWidgetClass = WidgetClass;
    }

    if (!AutoSaveIconWidgetClass.IsValid())
    {
        return;
    }

    if (!InstancedAutoSaveWidget.IsValid())
    {
        UWorld* World = GetWorld();
        if (World)
        {
            APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
            if (PC)
            {
                UUserWidget* Widget = CreateWidget<UUserWidget>(PC,
                    Cast<UClass>(AutoSaveIconWidgetClass.Get()));
                if (Widget)
                {
                    InstancedAutoSaveWidget = Widget;
                    Widget->AddToViewport(100);
                    Widget->SetVisibility(ESlateVisibility::Visible);
                    Widget->SetRenderOpacity(1.0f);
                }
            }
        }
    }

    UWorld* World = GetWorld();
    if (World)
    {
        UGameViewportClient* Viewport = World->GetGameViewport();
        if (Viewport && InstancedAutoSaveWidget.IsValid())
        {
            UUserWidget* Widget = Cast<UUserWidget>(InstancedAutoSaveWidget.Get());
            if (Widget)
            {
                TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
                if (SlateWidget.IsValid())
                {
                    Viewport->AddViewportWidgetContent(SlateWidget.ToSharedRef(), 1024);
                }
            }
        }
    }
}

void UFNAFSaveGameSystem::RemoveAutoSaveIcon()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UGameViewportClient* Viewport = World->GetGameViewport();

    if (InstancedAutoSaveWidget.IsValid())
    {
        UUserWidget* Widget = Cast<UUserWidget>(InstancedAutoSaveWidget.Get());
        if (Widget && Viewport)
        {
            TSharedPtr<SWidget> SlateWidget = Widget->GetCachedWidget();
            if (SlateWidget.IsValid())
            {
                Viewport->RemoveViewportWidgetContent(SlateWidget.ToSharedRef());
            }

            Widget->RemoveFromParent();
            Widget->Destruct();
        }

        InstancedAutoSaveWidget.Reset();
    }
}

void UFNAFSaveGameSystem::ToggleDLCCallToAction(bool visible)
{
    if (MasterDataObject)
    {
        MasterDataObject->RuinCallToActionVisible = visible;
        UGameplayStatics::SaveGameToSlot(MasterDataObject, MasterDataSlotName, UserIndex);
    }
}

bool UFNAFSaveGameSystem::DLCCallToActionVisibility()
{
    if (MasterDataObject)
    {
        return MasterDataObject->RuinCallToActionVisible;
    }
    return false;
}

bool UFNAFSaveGameSystem::IsSavingAllowed() const
{
    return bIsSavingAllowed;
}

void UFNAFSaveGameSystem::SetIsSavingAllowed(bool NewIsSavingAllowed)
{
    bIsSavingAllowed = NewIsSavingAllowed;
}

void UFNAFSaveGameSystem::SetInChapterReplay(bool inReplay, int32 Chapter)
{
    ReplayCurrentChapter = Chapter;
    bInChapterReplay = inReplay;
}

bool UFNAFSaveGameSystem::IsInChapterReplay()
{
    return bInChapterReplay;
}

void UFNAFSaveGameSystem::SetLoadIntoChapter(bool Load)
{
    LoadIntoChapter = Load;
    if (!Load)
    {
        UseAutosaveMapArea = false;
    }
}

bool UFNAFSaveGameSystem::GetLoadIntoChapter()
{
    return LoadIntoChapter;
}

void UFNAFSaveGameSystem::SetUserIndex(int32 NewUserIndex)
{
    UserIndex = NewUserIndex;
}

int32 UFNAFSaveGameSystem::GetUserIndex()
{
    return UserIndex;
}

void UFNAFSaveGameSystem::SetUseAutosaveMapArea(bool UseMapArea)
{
    UseAutosaveMapArea = UseMapArea;
}

bool UFNAFSaveGameSystem::GetUseAutosaveMapArea()
{
    return UseAutosaveMapArea;
}

void UFNAFSaveGameSystem::SetChowdaProfileIndex(int32 profileIndex)
{
    ChowdaProfileIndex = profileIndex;
}

int32 UFNAFSaveGameSystem::GetChowdaProfileIndex()
{
    return ChowdaProfileIndex;
}

int32 UFNAFSaveGameSystem::GetGameIteration() const
{
    if (SaveDataObject)
    {
        return SaveDataObject->GameIteration;
    }
    return 0;
}

int32 UFNAFSaveGameSystem::GetCurrentChapter()
{
    if (!SaveDataObject)
    {
        return 1;
    }

    if (bInChapterReplay)
    {
        return ReplayCurrentChapter;
    }

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        return ChowdaSave->Chapter;
    }

    return 1;
}

int32 UFNAFSaveGameSystem::GetLastSavedChapter()
{
    if (!MasterDataObject)
    {
        return 1;
    }

    USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(MasterDataObject->lastSavedChowdaSlotName, UserIndex);
    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(Loaded);
    if (ChowdaSave)
    {
        return ChowdaSave->Chapter;
    }

    return 1;
}

EMapArea UFNAFSaveGameSystem::GetMapAreaToLoad()
{
    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        return ChowdaSave->LastLoadedMapArea;
    }
    return EMapArea::Lobby;
}

EMapArea UFNAFSaveGameSystem::GetLastSavedMapArea()
{
    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        return ChowdaSave->LastLoadedMapArea;
    }
    return EMapArea::Lobby;
}

FString UFNAFSaveGameSystem::GetLastLoadedChowdaSlotName()
{
    if (MasterDataObject)
    {
        return MasterDataObject->lastLoadedChowdaSlotName;
    }
    return TEXT("");
}

bool UFNAFSaveGameSystem::HasSaveData() const
{
    if (!MasterDataObject)
    {
        return false;
    }
    return (MasterDataObject->SaveGameSlotNames_Map.Num() > 0);
}

bool UFNAFSaveGameSystem::HasPreviousSave()
{
    if (!MasterDataObject)
    {
        return false;
    }

    if (MasterDataObject->SaveGameSlotNames_Map.Num() > 0)
    {
        return GetHasSaveFromMapping(MasterDataObject->lastSavedSlotName);
    }

    return false;
}

bool UFNAFSaveGameSystem::HasAutoSave()
{
    return GetHasSaveFromMapping(AUTOSAVE_SLOT_NAME);
}

bool UFNAFSaveGameSystem::HasChowdaSaveGame(int32 ProfileIndexIn)
{
    FString SlotName = CHOWDA_AUTOSAVE_SLOT_NAME + FString::FromInt(ProfileIndexIn);
    return GetHasSaveFromMapping(SlotName);
}

bool UFNAFSaveGameSystem::HasActivitySave(const FString& InActivityId)
{
    if (!MasterDataObject)
    {
        return false;
    }

    FString* FoundSlot = MasterDataObject->ActivityIdSaveSlotNamesMap.Find(InActivityId);
    if (FoundSlot)
    {
        return GetHasSaveFromMapping(*FoundSlot);
    }

    return false;
}

bool UFNAFSaveGameSystem::PreviousSaveIsAuto()
{
    if (!MasterDataObject)
    {
        return false;
    }

    if (MasterDataObject->SaveGameSlotNames_Map.Num() <= 0)
    {
        return false;
    }

    if (!MasterDataObject->bLastSaveWasAuto)
    {
        return false;
    }

    return GetHasSaveFromMapping(AUTOSAVE_SLOT_NAME);
}

void UFNAFSaveGameSystem::Get_SaveSlotNameData(TMap<FString, FString>& SaveSlots_Map)
{
    if (!MasterDataObject)
    {
        MasterDataObject = Cast<UFNAFMasterData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFMasterData::StaticClass()));
    }

    if (MasterDataObject)
    {
        SaveSlots_Map = MasterDataObject->SaveGameSlotNames_Map;
    }
}

void UFNAFSaveGameSystem::Get_ChowdaSaveSlotNameData(TMap<FString, FString>& SaveSlots_Map)
{
    if (!MasterDataObject)
    {
        MasterDataObject = Cast<UFNAFMasterData>(
            UGameplayStatics::CreateSaveGameObject(UFNAFMasterData::StaticClass()));
    }

    if (MasterDataObject)
    {
        SaveSlots_Map = MasterDataObject->ChowdaSaveGameSlotNames_Map;
    }
}
