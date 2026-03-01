#include "FNAFInventorySystem.h"
#include "Engine/DataTable.h"
#include "FNAFChowdaSaveData.h"
#include "FNAFGameInstanceBase.h"
#include "FNAFBasePlayerController.h"
#include "FNAFSaveData.h"
#include "FNAFSaveGameSystem.h"
#include "GameClockSystem.h"
#include "Kismet/GameplayStatics.h"

FName UFNAFInventorySystem::FlashBeaconItemName = FName(TEXT("Flashbeacon"));

UFNAFInventorySystem::UFNAFInventorySystem()
    : InventoryDataTable(nullptr)
    , MessageDataTable(nullptr)
    , FazwatchPowerLevel(0)
    , FazwatchMaxPowerLevel(0)
    , FreddyPowerLevel(0)
    , FreddyMaxPowerLevel(0)
    , SecurityLevel(0)
    , CollectedPartyPassCount(0)
    , UsedPartyPassCount(0)
    , NumAvailableFlash(0)
    , bUnlimitedFazwatchPower(false)
    , bUnlimitedStamina(false)
    , NumDishesBroken(0)
    , CollectedCautionBotSouls(0)
    , InitialFreddyMaxPower(100)
    , InitialFlashlightMaxPower(300)
    , FlashlightStationID(-1)
{
    static ConstructorHelpers::FObjectFinder<UDataTable> InventoryDataObject(TEXT("/Game/Data/InventoryDataTable.InventoryDataTable")); // placeholder path
    if (InventoryDataObject.Object)
    {
        InventoryDataTable = InventoryDataObject.Object;
    }

    static ConstructorHelpers::FObjectFinder<UDataTable> MessageDataObject(TEXT("/Game/Data/MessageDataTable.MessageDataTable")); // placeholder path
    if (MessageDataObject.Object)
    {
        MessageDataTable = MessageDataObject.Object;
    }

    FazwatchMaxPowerLevel = InitialFlashlightMaxPower;
    FazwatchPowerLevel = InitialFlashlightMaxPower;
    FreddyMaxPowerLevel = InitialFreddyMaxPower;
    FreddyPowerLevel = InitialFreddyMaxPower;
}

// ISaveHandlerInterface

void UFNAFInventorySystem::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    FFNAFInventorySaveData& InvSave = SaveDataObject->InventorySaveData;

    SaveDataObject->FazwatchPowerSaveData.PowerLevel = FazwatchPowerLevel;
    SaveDataObject->FazwatchPowerSaveData.MaxPowerLevel = FazwatchMaxPowerLevel;
    SaveDataObject->FreddyPowerSaveData.PowerLevel = FreddyPowerLevel;
    SaveDataObject->FreddyPowerSaveData.MaxPowerLevel = FreddyMaxPowerLevel;

    InvSave.SecurityLevel = SecurityLevel;
    InvSave.UsedPartyPassCount = UsedPartyPassCount;
    InvSave.NumFlashCharges = NumAvailableFlash;
    InvSave.FlashlightInStationID = FlashlightStationID;
    InvSave.TapesListenedTo = TapesListenedTo;
    InvSave.DishesBroken = NumDishesBroken;

    InvSave.Messages = MessagesOwned;

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        ChowdaSave->CollectedCautionBotSouls = CollectedCautionBotSouls;
        ChowdaSave->CandyCadetStoriesDone = CandyCadetStoriesDone;

        if (ChowdaSave->bInChapterReplay)
        {
            InvSave.InventoryItems = GetMergedInventoryItems();
        }
        else if (InventoryItemsOwned.Num() > 0)
        {
            InvSave.InventoryItems = GetMergedInventoryItems();
            LoadedInventoryItemsOwned = InventoryItemsOwned;
        }
    }
    else
    {
        InvSave.InventoryItems = GetMergedInventoryItems();
    }
}

void UFNAFInventorySystem::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    const FFNAFInventorySaveData& InvSave = SaveDataObject->InventorySaveData;

    InventoryItemsOwned = InvSave.InventoryItems;
    MessagesOwned = InvSave.Messages;

    LoadedInventoryItemsOwned = InventoryItemsOwned;

    FazwatchPowerLevel = SaveDataObject->FazwatchPowerSaveData.PowerLevel;
    FazwatchMaxPowerLevel = SaveDataObject->FazwatchPowerSaveData.MaxPowerLevel;
    FreddyPowerLevel = SaveDataObject->FreddyPowerSaveData.PowerLevel;
    FreddyMaxPowerLevel = SaveDataObject->FreddyPowerSaveData.MaxPowerLevel;

    SecurityLevel = InvSave.SecurityLevel;
    UsedPartyPassCount = InvSave.UsedPartyPassCount;
    NumAvailableFlash = InvSave.NumFlashCharges;
    FlashlightStationID = InvSave.FlashlightInStationID;
    TapesListenedTo = InvSave.TapesListenedTo;
    NumDishesBroken = InvSave.DishesBroken;

    // Recount collected party passes from loaded inventory
    CollectedPartyPassCount = 0;
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        FFNAFInventoryTableStruct ItemInfo;
        bool bFound = false;
        GetItemInfo(Item.ItemName, ItemInfo, bFound);
        if (bFound && ItemInfo.Category == EInventoryItemCategory::PartyBadge)
        {
            CollectedPartyPassCount++;
        }
    }

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        CollectedCautionBotSouls = ChowdaSave->CollectedCautionBotSouls;
        CandyCadetStoriesDone = ChowdaSave->CandyCadetStoriesDone;
    }
}

void UFNAFInventorySystem::OnCheckpointSave_Implementation(UFNAFSaveData* SaveDataObject)
{
    OnStoreGameData_Implementation(SaveDataObject);
}

void UFNAFInventorySystem::OnCheckpointLoad_Implementation(UFNAFSaveData* SaveDataObject)
{
    OnGameDataLoaded_Implementation(SaveDataObject);
}

// Setup / Reset

void UFNAFInventorySystem::SetupNewGame()
{
    FazwatchMaxPowerLevel = InitialFlashlightMaxPower;
    FazwatchPowerLevel = InitialFlashlightMaxPower;
    FreddyMaxPowerLevel = InitialFreddyMaxPower;
    FreddyPowerLevel = InitialFreddyMaxPower;
    NumDishesBroken = 0;
    SecurityLevel = 0;
    CollectedPartyPassCount = 0;
    UsedPartyPassCount = 0;

    InventoryItemsOwned.Empty();
    LoadedInventoryItemsOwned.Empty();
    MessagesOwned.Empty();
    TapesListenedTo.Empty();
}

void UFNAFInventorySystem::SetupGameMode(EFNAFGameType GameType)
{
}

void UFNAFInventorySystem::SetupDataTable(UDataTable* InDataTable)
{
    if (InDataTable)
    {
        InventoryDataTable = InDataTable;
    }
}

void UFNAFInventorySystem::ResetOnExitToMenu()
{
    LoadedInventoryItemsOwned.Empty();
}

void UFNAFInventorySystem::RemoveEverything()
{
    InventoryItemsOwned.Empty();
    MessagesOwned.Empty();
    LoadedInventoryItemsOwned.Empty();
}

void UFNAFInventorySystem::ResetForChapterSelect(TArray<FName> CollectedItems, int32 chapterSelected)
{
    LoadedInventoryItemsOwned.Empty();
    LoadedInventoryItemsOwned.Append(InventoryItemsOwned);

    // Remove Equipment items at or above the selected chapter
    for (int32 i = 0; i < InventoryItemsOwned.Num(); i++)
    {
        FFNAFInventoryTableStruct ItemInfo;
        bool bFound = false;
        GetItemInfo(InventoryItemsOwned[i].ItemName, ItemInfo, bFound);
        if (bFound && ItemInfo.Category == EInventoryItemCategory::Equipment && ItemInfo.Chapter >= chapterSelected)
        {
            InventoryItemsOwned.RemoveAt(i);
            i--;
        }
    }

    // Award any items from CollectedItems that aren't already in inventory
    for (const FName& ItemName : CollectedItems)
    {
        if (!HasItem(ItemName))
        {
            AwardItem(ItemName, true);
        }
    }
}

// Item Award / Remove

void UFNAFInventorySystem::AwardItem(FName Item, bool bNotify)
{
    if (Item == NAME_None)
    {
        return;
    }

    FFNAFInventoryTableStruct ItemInfo;
    bool bFound = false;
    GetItemInfo(Item, ItemInfo, bFound);
    if (!bFound)
    {
        return;
    }

    if (ItemInfo.Category == EInventoryItemCategory::SecurityBadge)
    {
        int32 NewLevel = ++SecurityLevel;
        if (bNotify)
        {
            OnSecurityLevelUpdated.Broadcast(NewLevel);
        }
    }
    else if (ItemInfo.Category == EInventoryItemCategory::PartyBadge)
    {
        int32 NewCount = ++CollectedPartyPassCount;
        if (bNotify)
        {
            OnPartyLevelUpdated.Broadcast(NewCount);
        }
    }

    AddItemToArray(InventoryItemsOwned, Item);

    // Check for upgrade items and apply their effects
    FString ItemNameStr = Item.ToString();

    if (ItemNameStr.Contains(TEXT("FlashlightUpgrade")))
    {
        SetPowerLevel_Fazwatch(GetFlashlightMaxPower());
    }
    else if (!ItemNameStr.Contains(TEXT("FreddyUpgrade")))
    {
        FString FlashBeaconStr = FlashBeaconItemName.ToString();
        if (ItemNameStr.Contains(FlashBeaconStr))
        {
            NumAvailableFlash = CountFlashBeaconUpgrades();
        }
    }

    if (bNotify && OnInventoryItemAdded.IsBound())
    {
        OnInventoryItemAdded.Broadcast(Item, ItemInfo);
    }

    // Check if all VIP prizes have been collected (only in normal play)
    UGameInstance* GI = GetGameInstance();
    UFNAFGameInstanceBase* FNAFGameInstance = Cast<UFNAFGameInstanceBase>(GI);
    if (FNAFGameInstance && FNAFGameInstance->IsNormalPlay())
    {
        CheckIfAllPrizesHaveBeenCollected();
    }
}

void UFNAFInventorySystem::RemoveItem(FName Item)
{
    for (int32 i = 0; i < InventoryItemsOwned.Num(); i++)
    {
        if (InventoryItemsOwned[i].ItemName == Item)
        {
            FFNAFInventoryTableStruct ItemInfo;
            bool bFound = false;
            GetItemInfo(Item, ItemInfo, bFound);
            if (bFound)
            {
                if (ItemInfo.Category == EInventoryItemCategory::SecurityBadge)
                {
                    SecurityLevel--;
                }
                else if (ItemInfo.Category == EInventoryItemCategory::PartyBadge)
                {
                    CollectedPartyPassCount--;
                }
            }

            InventoryItemsOwned.RemoveAt(i);

            if (OnInventoryItemRemoved.IsBound())
            {
                OnInventoryItemRemoved.Broadcast(Item, ItemInfo);
            }
            break;
        }
    }

    for (int32 i = 0; i < LoadedInventoryItemsOwned.Num(); i++)
    {
        if (LoadedInventoryItemsOwned[i].ItemName == Item)
        {
            LoadedInventoryItemsOwned.RemoveAt(i);
            break;
        }
    }
}

void UFNAFInventorySystem::AwardMessage(FName Message, bool bNotify)
{
    if (Message == NAME_None)
    {
        return;
    }

    FFNAFMessageTableStruct MessageInfo;
    bool bFound = false;
    GetMessageInfo(Message, MessageInfo, bFound);
    if (!bFound)
    {
        return;
    }

    AddItemToArray(MessagesOwned, Message);

    if (bNotify && OnMessageAdded.IsBound())
    {
        OnMessageAdded.Broadcast(Message, MessageInfo);
    }
}

void UFNAFInventorySystem::RemoveMessage(FName Message)
{
    for (int32 i = 0; i < MessagesOwned.Num(); i++)
    {
        if (MessagesOwned[i].ItemName == Message)
        {
            MessagesOwned.RemoveAt(i);
            return;
        }
    }
}

void UFNAFInventorySystem::AddEverything()
{
    if (!InventoryDataTable || !MessageDataTable)
    {
        return;
    }

    TArray<FName> InventoryRowNames = InventoryDataTable->GetRowNames();
    for (const FName& RowName : InventoryRowNames)
    {
        AwardItem(RowName, false);
    }

    TArray<FName> MessageRowNames = MessageDataTable->GetRowNames();
    for (const FName& RowName : MessageRowNames)
    {
        AwardMessage(RowName, false);
    }
}

void UFNAFInventorySystem::AddItemToArray(TArray<FFNAFItemCollectInfo>& ItemArray, FName ItemName)
{
    for (const FFNAFItemCollectInfo& Existing : ItemArray)
    {
        if (Existing.ItemName == ItemName)
        {
            return; // Already present, don't add duplicate
        }
    }

    FFNAFItemCollectInfo NewEntry;
    NewEntry.ItemName = ItemName;
    NewEntry.HasViewed = false;

    UFNAFSaveGameSystem* SaveSystem = GetGameInstance()->GetSubsystem<UFNAFSaveGameSystem>();
    UGameClockSystem* ClockSystem = GetGameInstance()->GetSubsystem<UGameClockSystem>();

    if (SaveSystem && ClockSystem)
    {
        float CurrentGameTime = ClockSystem->GetCurrentTimeInSeconds();
        if (CurrentGameTime >= 0.0f)
        {
            int32 TotalMinutes = (int32)CurrentGameTime / 60;
            NewEntry.GameHour = TotalMinutes / 60;
            NewEntry.GameMinute = TotalMinutes % 60;
        }
        else
        {
            NewEntry.GameHour = -1;
            NewEntry.GameMinute = (int32)(CurrentGameTime / 60.0f + 60.0f);
        }

        NewEntry.PlayIteration = SaveSystem->GetGameIteration();
    }
    else
    {
        NewEntry.GameHour = 0;
        NewEntry.GameMinute = 0;
        NewEntry.PlayIteration = 0;
    }

    NewEntry.CollectionTime = FDateTime::Now().GetTicks();

    ItemArray.Add(NewEntry);
}

float UFNAFInventorySystem::GetMultiplierForItemCount(float PerItemMultiplier, const FString& NameContains) const
{
    int32 Count = 0;
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        FString ItemStr = Item.ItemName.ToString();
        if (ItemStr.Contains(NameContains))
        {
            Count++;
        }
    }
    return FMath::Pow(PerItemMultiplier, (float)Count);
}

int32 UFNAFInventorySystem::CountFlashBeaconUpgrades() const
{
    if (!HasItem(FlashBeaconItemName))
    {
        return 0;
    }

    int32 Count = 1; // Base flash beacon counts as 1
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        FString ItemStr = Item.ItemName.ToString();
        if (ItemStr.Contains(TEXT("FlashbeaconUpgrade")))
        {
            Count++;
        }
    }
    return Count;
}

// Merges current items into loaded (avoiding duplicates by FName)
TArray<FFNAFItemCollectInfo> UFNAFInventorySystem::GetMergedInventoryItems()
{
    for (const FFNAFItemCollectInfo& CurrentItem : InventoryItemsOwned)
    {
        bool bAlreadyInLoaded = false;
        for (const FFNAFItemCollectInfo& LoadedItem : LoadedInventoryItemsOwned)
        {
            if (LoadedItem.ItemName == CurrentItem.ItemName)
            {
                bAlreadyInLoaded = true;
                break;
            }
        }
        if (!bAlreadyInLoaded)
        {
            LoadedInventoryItemsOwned.Add(CurrentItem);
        }
    }

    return LoadedInventoryItemsOwned;
}

void UFNAFInventorySystem::CheckIfAllPrizesHaveBeenCollected()
{
    if (!InventoryDataTable)
    {
        return;
    }

    TArray<FName> VIPRowNames;
    const TMap<FName, uint8*>& RowMap = InventoryDataTable->GetRowMap();
    for (const auto& Pair : RowMap)
    {
        const uint8* RowData = Pair.Value;
        if (RowData)
        {
            const FFNAFInventoryTableStruct* Row = reinterpret_cast<const FFNAFInventoryTableStruct*>(RowData);
            if (Row->PrizeVIP)
            {
                VIPRowNames.Add(Pair.Key);
            }
        }
    }

    TArray<FName> PlayerItemNames;
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        PlayerItemNames.Add(Item.ItemName);
    }

    for (const FName& VIPName : VIPRowNames)
    {
        if (!PlayerItemNames.Contains(VIPName))
        {
            return; // Missing at least one VIP item
        }
    }

    // All VIP prizes collected
    APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    AFNAFBasePlayerController* FNAFPC = Cast<AFNAFBasePlayerController>(PC);
    if (FNAFPC)
    {
        FNAFPC->GiveVIPAchievement();
    }
}

// Power Management

bool UFNAFInventorySystem::AdjustPower_Fazwatch(int32 ChangeAmount)
{
    const int32 MaxPower = GetFlashlightMaxPower();

    if (bUnlimitedFazwatchPower)
    {
        FazwatchPowerLevel = MaxPower;
        return true;
    }

    int32 NewLevel = FazwatchPowerLevel + ChangeAmount;
    if (NewLevel < 0)
    {
        FazwatchPowerLevel = 0;
        return false;
    }
    else if (NewLevel > MaxPower)
    {
        FazwatchPowerLevel = MaxPower;
        return false;
    }
    else
    {
        FazwatchPowerLevel = NewLevel;
        return true;
    }
}

bool UFNAFInventorySystem::AdjustPower_Freddy(int32 ChangeAmount)
{
    const int32 MaxPower = GetFreddyMaxPower();

    int32 NewLevel = FreddyPowerLevel + ChangeAmount;
    if (NewLevel < 0)
    {
        FreddyPowerLevel = 0;
        return false;
    }
    else if (NewLevel > MaxPower)
    {
        FreddyPowerLevel = MaxPower;
        return false;
    }
    else
    {
        FreddyPowerLevel = NewLevel;
        return true;
    }
}

void UFNAFInventorySystem::SetPowerLevel_Freddy(int32 InPowerLevel)
{
    const int32 MaxPower = GetFreddyMaxPower();
    if (InPowerLevel < 0)
    {
        FreddyPowerLevel = 0;
    }
    else
    {
        FreddyPowerLevel = FMath::Min(InPowerLevel, MaxPower);
    }
}

void UFNAFInventorySystem::SetPowerLevel_Fazwatch(int32 InPowerLevel)
{
    const int32 MaxPower = GetFlashlightMaxPower();
    if (InPowerLevel < 0)
    {
        FazwatchPowerLevel = 0;
    }
    else
    {
        FazwatchPowerLevel = FMath::Min(InPowerLevel, MaxPower);
    }
}

void UFNAFInventorySystem::SetMaxPowerLevel_Freddy(int32 NewMax)
{
    FreddyMaxPowerLevel = NewMax;
}

void UFNAFInventorySystem::SetMaxPowerLevel_Fazwatch(int32 NewMax)
{
    FazwatchMaxPowerLevel = NewMax;
}

int32 UFNAFInventorySystem::GetFlashlightMaxPower() const
{
    float Multiplier = GetMultiplierForItemCount(1.25f, TEXT("FlashlightUpgrade"));
    return (int32)(FazwatchMaxPowerLevel * Multiplier);
}

int32 UFNAFInventorySystem::GetFreddyMaxPower() const
{
    float Multiplier = GetMultiplierForItemCount(1.5f, TEXT("FreddyUpgrade"));
    return (int32)(FreddyMaxPowerLevel * Multiplier);
}

float UFNAFInventorySystem::GetFlashlightUpgradMultiplier() const
{
    return GetMultiplierForItemCount(1.25f, TEXT("FlashlightUpgrade"));
}

float UFNAFInventorySystem::GetFreddyUpgradeMutliplier() const
{
    return GetMultiplierForItemCount(1.5f, TEXT("FreddyUpgrade"));
}

float UFNAFInventorySystem::GetStaminaUpgradeMultiplier() const
{
    return GetMultiplierForItemCount(1.25f, TEXT("GregoryUpgrade_Stamina"));
}

bool UFNAFInventorySystem::HasEnoughPower_Fazwatch(int32 PowerRequired) const
{
    return bUnlimitedFazwatchPower || FazwatchPowerLevel >= PowerRequired;
}

bool UFNAFInventorySystem::HasEnoughPower_Freddy(int32 PowerRequired) const
{
    return FreddyPowerLevel >= PowerRequired;
}

void UFNAFInventorySystem::ResetFlashlightPower()
{
    FazwatchPowerLevel = GetFlashlightMaxPower();
}

void UFNAFInventorySystem::ResetFreddyPower()
{
    FreddyPowerLevel = GetFreddyMaxPower();
}

// Flash Beacon

bool UFNAFInventorySystem::CanUseFlashBeacon() const
{
    return HasItem(FlashBeaconItemName) && NumAvailableFlash > 0;
}

void UFNAFInventorySystem::UseFlash(bool& bOutFlashUsed)
{
    if (HasItem(FlashBeaconItemName) && NumAvailableFlash > 0)
    {
        bOutFlashUsed = true;
        NumAvailableFlash--;
    }
    else
    {
        bOutFlashUsed = false;
    }
}

int32 UFNAFInventorySystem::GetMaxFlashes() const
{
    return CountFlashBeaconUpgrades();
}

void UFNAFInventorySystem::ResetFlashes()
{
    if (HasItem(FlashBeaconItemName))
    {
        NumAvailableFlash = CountFlashBeaconUpgrades();
    }
    else
    {
        NumAvailableFlash = 0;
    }
}

// Flashlight Station

void UFNAFInventorySystem::SetFlashlightStationID(int32 StationID)
{
    FlashlightStationID = StationID;
}

int32 UFNAFInventorySystem::GetFlashlightStationID() const
{
    return FlashlightStationID;
}

void UFNAFInventorySystem::ClearFlashlightStationID()
{
    FlashlightStationID = -1;
}

// Security & Party

void UFNAFInventorySystem::SetSecurityLevel(int32 NewSecurityLevel)
{
    SecurityLevel = NewSecurityLevel;
}

bool UFNAFInventorySystem::HasSecurityClearance(int32 InSecurityLevel) const
{
    return SecurityLevel >= InSecurityLevel;
}

void UFNAFInventorySystem::SetPartyLevel(int32 NewPartyLevel)
{
    CollectedPartyPassCount = NewPartyLevel;
}

bool UFNAFInventorySystem::HasAvailablePartyPass() const
{
    return (CollectedPartyPassCount - UsedPartyPassCount) > 0;
}

bool UFNAFInventorySystem::UsePartyPass()
{
    if (CollectedPartyPassCount - UsedPartyPassCount <= 0)
    {
        return false;
    }

    UsedPartyPassCount++;

    if (OnPartyPassUsed.IsBound())
    {
        OnPartyPassUsed.Broadcast(CollectedPartyPassCount - UsedPartyPassCount);
    }

    return true;
}

TArray<FName> UFNAFInventorySystem::GetCollectedPartyPasses() const
{
    TArray<FName> Result;
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        FFNAFInventoryTableStruct ItemInfo;
        bool bFound = false;
        const_cast<UFNAFInventorySystem*>(this)->GetItemInfo(Item.ItemName, ItemInfo, bFound);
        if (bFound && ItemInfo.Category == EInventoryItemCategory::PartyBadge)
        {
            Result.Add(Item.ItemName);
        }
    }
    return Result;
}

// Caution Bot Souls

int32 UFNAFInventorySystem::AwardCautionBotSoul()
{
    CollectedCautionBotSouls++;

    if (OnCautionBotSoulCountUpdated.IsBound())
    {
        OnCautionBotSoulCountUpdated.Broadcast(CollectedCautionBotSouls);
    }

    return CollectedCautionBotSouls;
}

int32 UFNAFInventorySystem::RemoveCautionBotSouls(int32 NumberOfSouls)
{
    CollectedCautionBotSouls = FMath::Max(0, CollectedCautionBotSouls - NumberOfSouls);

    if (OnCautionBotSoulCountUpdated.IsBound())
    {
        OnCautionBotSoulCountUpdated.Broadcast(CollectedCautionBotSouls);
    }

    return CollectedCautionBotSouls;
}

int32 UFNAFInventorySystem::SetCautionBotSoulsCollectedNumber(int32 inNumber)
{
    CollectedCautionBotSouls = inNumber;

    if (OnCautionBotSoulCountUpdated.IsBound())
    {
        OnCautionBotSoulCountUpdated.Broadcast(CollectedCautionBotSouls);
    }

    return CollectedCautionBotSouls;
}

int32 UFNAFInventorySystem::GetCautionBotSoulsCollectedNumber()
{
    return CollectedCautionBotSouls;
}

// Dishes / Plates

int32 UFNAFInventorySystem::AddBrokenPlate()
{
    return ++NumDishesBroken;
}

// Tapes

void UFNAFInventorySystem::AddTapeListenedTo(FName InTapeListenedTo)
{
    TapesListenedTo.Add(InTapeListenedTo);
}

TSet<FName> UFNAFInventorySystem::GetTapesListenedTo() const
{
    return TapesListenedTo;
}

// Candy Cadet Stories

void UFNAFInventorySystem::AddCandyCadetStoryDone(FName InStory)
{
    if (InStory != NAME_None)
    {
        CandyCadetStoriesDone.Add(InStory);
    }
}

TSet<FName> UFNAFInventorySystem::GetCandyCadetStoriesDone() const
{
    return CandyCadetStoriesDone;
}

// Item Queries

bool UFNAFInventorySystem::HasItem(FName searchItem) const
{
    if (searchItem == NAME_None)
    {
        return false;
    }

    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        if (Item.ItemName == searchItem)
        {
            return true;
        }
    }
    return false;
}

bool UFNAFInventorySystem::HasMessage(FName SearchMessage) const
{
    if (SearchMessage == NAME_None)
    {
        return false;
    }

    for (const FFNAFItemCollectInfo& Msg : MessagesOwned)
    {
        if (Msg.ItemName == SearchMessage)
        {
            return true;
        }
    }
    return false;
}

bool UFNAFInventorySystem::HasItemBeenViewed(FName ItemOrMessage) const
{
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        if (Item.ItemName == ItemOrMessage)
        {
            return Item.HasViewed;
        }
    }
    return false;
}

bool UFNAFInventorySystem::HasMessageBeenViewed(FName ItemOrMessage) const
{
    for (const FFNAFItemCollectInfo& Msg : MessagesOwned)
    {
        if (Msg.ItemName == ItemOrMessage)
        {
            return Msg.HasViewed;
        }
    }
    return false;
}

void UFNAFInventorySystem::SetItemViewed(FName Item)
{
    for (FFNAFItemCollectInfo& Entry : InventoryItemsOwned)
    {
        if (Entry.ItemName == Item)
        {
            Entry.HasViewed = true;
            return;
        }
    }
}

void UFNAFInventorySystem::SetMessageViewed(FName Message)
{
    for (FFNAFItemCollectInfo& Entry : MessagesOwned)
    {
        if (Entry.ItemName == Message)
        {
            Entry.HasViewed = true;
            return;
        }
    }
}

bool UFNAFInventorySystem::IsItemValid(FName Item) const
{
    FFNAFInventoryTableStruct ItemInfo;
    bool bFound = false;
    const_cast<UFNAFInventorySystem*>(this)->GetItemInfo(Item, ItemInfo, bFound);
    return bFound;
}

bool UFNAFInventorySystem::IsMessageValid(FName Message) const
{
    FFNAFMessageTableStruct MessageInfo;
    bool bFound = false;
    const_cast<UFNAFInventorySystem*>(this)->GetMessageInfo(Message, MessageInfo, bFound);
    return bFound;
}

bool UFNAFInventorySystem::IsVIPItem(FName Item) const
{
    if (Item == NAME_None)
    {
        return false;
    }

    FFNAFInventoryTableStruct ItemInfo;
    bool bFound = false;
    const_cast<UFNAFInventorySystem*>(this)->GetItemInfo(Item, ItemInfo, bFound);
    if (bFound)
    {
        return ItemInfo.PrizeVIP;
    }
    return false;
}

// Data Table Lookups

void UFNAFInventorySystem::GetItemInfo(FName ItemName, FFNAFInventoryTableStruct& OutItemInfo, bool& OutFound) const
{
    OutFound = false;

    if (!InventoryDataTable)
    {
        OutItemInfo = FFNAFInventoryTableStruct();
        return;
    }

    static const FString ContextString(TEXT("InventorySystem"));
    const FFNAFInventoryTableStruct* Row = InventoryDataTable->FindRow<FFNAFInventoryTableStruct>(ItemName, ContextString, true);
    if (Row)
    {
        OutItemInfo = *Row;
        OutFound = true;
    }
    else
    {
        OutItemInfo = FFNAFInventoryTableStruct();
    }
}

void UFNAFInventorySystem::GetMessageInfo(FName MessageName, FFNAFMessageTableStruct& OutMessageInfo, bool& OutFound) const
{
    OutFound = false;

    if (!MessageDataTable)
    {
        OutMessageInfo = FFNAFMessageTableStruct();
        return;
    }

    static const FString ContextString(TEXT("GetMessageInfo"));
    const FFNAFMessageTableStruct* Row = MessageDataTable->FindRow<FFNAFMessageTableStruct>(MessageName, ContextString, true);
    if (Row)
    {
        OutMessageInfo = *Row;
        OutFound = true;
    }
    else
    {
        OutMessageInfo = FFNAFMessageTableStruct();
    }
}

TArray<FName> UFNAFInventorySystem::GetAllItems() const
{
    TArray<FName> Result;
    if (InventoryDataTable)
    {
        Result = InventoryDataTable->GetRowNames();
    }
    return Result;
}

TArray<FName> UFNAFInventorySystem::GetAllCurrentInventoryItems()
{
    TArray<FName> Result;
    for (const FFNAFItemCollectInfo& Item : InventoryItemsOwned)
    {
        Result.Add(Item.ItemName);
    }
    return Result;
}

TArray<FName> UFNAFInventorySystem::GetAllSurvivalItemsOfType(EInventoryItemSurvivalCategory SurvivalCategory) const
{
    TArray<FName> Result;

    if (!InventoryDataTable)
    {
        return Result;
    }

    TArray<FName> RowNames = InventoryDataTable->GetRowNames();
    static const FString ContextString(TEXT("GetAllSurvivalItemsOfType"));

    for (const FName& RowName : RowNames)
    {
        const FFNAFInventoryTableStruct* Row = InventoryDataTable->FindRow<FFNAFInventoryTableStruct>(RowName, ContextString, false);
        if (Row && Row->SurvivalCategory == SurvivalCategory)
        {
            Result.Add(RowName);
        }
    }

    return Result;
}

/* Despite the function name, the original binary counts messages where
   HasViewed is TRUE (viewed). The function name may be misleading. */
int32 UFNAFInventorySystem::GetNumberOfUnreadMessages() const
{
    int32 Count = 0;
    for (const FFNAFItemCollectInfo& Msg : MessagesOwned)
    {
        if (Msg.HasViewed)
        {
            Count++;
        }
    }
    return Count;
}

// Chapter Progression

float UFNAFInventorySystem::GetChapterProgression(int32 Chapter, UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject || !InventoryDataTable)
    {
        return 0.0f;
    }

    // Count total collectible items for this chapter (CollectibleDLC, RealCollectables, ARCollectables)
    int32 TotalForChapter = 0;
    TArray<FName> RowNames = InventoryDataTable->GetRowNames();
    static const FString ContextString(TEXT("GetChapterProgression"));

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);

    for (const FName& RowName : RowNames)
    {
        const FFNAFInventoryTableStruct* Row = InventoryDataTable->FindRow<FFNAFInventoryTableStruct>(RowName, ContextString, false);
        if (Row)
        {
            bool bIsCollectible = (Row->Category == EInventoryItemCategory::CollectibleDLC
                || Row->Category == EInventoryItemCategory::RealCollectables
                || Row->Category == EInventoryItemCategory::ARCollectables);

            if (bIsCollectible && Row->Chapter == Chapter && ChowdaSave)
            {
                TotalForChapter++;
            }
        }
    }

    // Count how many of those the player has collected (from save data)
    int32 CollectedCount = 0;
    const TArray<FFNAFItemCollectInfo>& SaveGameItems = SaveDataObject->InventorySaveData.InventoryItems;
    for (const FFNAFItemCollectInfo& SaveItem : SaveGameItems)
    {
        FFNAFInventoryTableStruct ItemInfo;
        bool bFound = false;
        GetItemInfo(SaveItem.ItemName, ItemInfo, bFound);
        if (bFound)
        {
            bool bIsCollectible = (ItemInfo.Category == EInventoryItemCategory::CollectibleDLC
                || ItemInfo.Category == EInventoryItemCategory::RealCollectables
                || ItemInfo.Category == EInventoryItemCategory::ARCollectables);
            if (bIsCollectible && ItemInfo.Chapter == Chapter)
            {
                CollectedCount++;
            }
        }
    }

    if (TotalForChapter <= 0)
    {
        return 1.0f;
    }

    float Ratio = (float)CollectedCount / (float)TotalForChapter;
    return FMath::Clamp(Ratio, 0.0f, 1.0f);
}
