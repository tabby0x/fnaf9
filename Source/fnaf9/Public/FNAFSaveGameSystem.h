#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/SubsystemCollection.h"
#include "EMapArea.h"
#include "GameSaveDelegateDelegate.h"
#include "FNAFSaveGameSystem.generated.h"

class UFNAFMasterData;
class UFNAFSaveData;
class UFNAFChowdaSaveData;
class UUserWidget;
class USaveGame;

UCLASS(Blueprintable)
class FNAF9_API UFNAFSaveGameSystem : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
private:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    UFNAFSaveData* SaveDataObject;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    UFNAFSaveData* TempSaveDataObject;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    UFNAFSaveData* WorldTransitDataObject;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    UFNAFMasterData* MasterDataObject;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 MaxNumOfSaveSlots;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    float LoadedGameTime;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    bool SaveSlotsInitalized;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    bool bChowdaSaveSlotsInitialized;

    UPROPERTY(Transient)
    TArray<AActor*> SaveActorClasses;

    UPROPERTY(Transient)
    TArray<UObject*> GameInstanceSubsystemSaveArray;

    UPROPERTY(Transient)
    TArray<UObject*> WorldSubsystemSaveArray;

    UPROPERTY(Transient)
    TArray<UObject*> GISCheckpointArray;

    UPROPERTY(Transient)
    TArray<UObject*> WSCheckpointArray;

public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FGameSaveDelegate OnSaveGameBegin;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FGameSaveDelegate OnSaveGameComplete;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FGameSaveDelegate OnLoadGameComplete;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FGameSaveDelegate AutoSaveTriggered;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TMap<FString, FString> SaveSlotNames_Data;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FString MasterDataSlotName;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FString SaveSlotName;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FString ChowdaSaveSlotName;

    /* TWeakObjectPtr, NOT UPROPERTY for UClass* */
    TWeakObjectPtr<UObject> AutoSaveIconWidgetClass;
    TWeakObjectPtr<UObject> InstancedAutoSaveWidget;

    UPROPERTY(Transient)
    TMap<FString, bool> HasSaveSlotMapping;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bIsSavingAllowed;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bInvertedGamepad;

    UPROPERTY(EditAnywhere, meta = (AllowPrivateAccess = true))
    uint32 UserIndex;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 ChowdaProfileIndex;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 MaxChowdaSaveSlots;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    bool bDoesSaveGameExist;

    bool TakenSlots[50];

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    bool bInChapterReplay;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    int32 ReplayCurrentChapter;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    EMapArea MapAreaOnLastAutosave;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    bool UseAutosaveMapArea;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Transient, meta = (AllowPrivateAccess = true))
    bool LoadIntoChapter;

    static FString CHOWDA_AUTOSAVE_SLOT_NAME;
    static FString AUTOSAVE_SLOT_NAME;
    static FString WORLD_TRANSIT_SLOT_NAME;

    UFNAFSaveGameSystem();
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    UFUNCTION(BlueprintCallable)
    void ToggleDLCCallToAction(bool visible);

    UFUNCTION(BlueprintCallable)
    void ShowAutoSaveIcon();

    UFUNCTION(BlueprintCallable)
    void SetUserIndex(int32 NewUserIndex);

    UFUNCTION(BlueprintCallable)
    void SetUseAutosaveMapArea(bool UseMapArea);

    UFUNCTION(BlueprintCallable)
    void SetupPIE();

    UFUNCTION(BlueprintCallable)
    void SetupNewGameOnSlot(const FString& originalSlotName);

    UFUNCTION(BlueprintCallable)
    void SetupNewGame();

    UFUNCTION(BlueprintCallable)
    void SetupNewChowdaGame(int32 ProfileIndexIn);

    UFUNCTION(BlueprintCallable)
    void SetLoadIntoChapter(bool Load);

    UFUNCTION(BlueprintCallable)
    void SetIsSavingAllowed(bool NewIsSavingAllowed);

    UFUNCTION(BlueprintCallable)
    void SetInChapterReplay(bool inReplay, int32 Chapter);

    UFUNCTION(BlueprintCallable)
    void SetChowdaProfileIndex(int32 profileIndex);

    UFUNCTION(BlueprintCallable)
    void SaveCurrentGame();

    UFUNCTION(BlueprintCallable)
    void SaveCheckpoint();

    UFUNCTION(BlueprintCallable)
    void SaveArcade();

    UFUNCTION(BlueprintCallable)
    void Reset_SaveSlots();

    UFUNCTION(BlueprintCallable)
    void Rename_SaveSlot(const FString& originalSlotName, const FString& renamedSlotName);

    UFUNCTION(BlueprintCallable)
    void RemoveAutoSaveIcon();

    UFUNCTION(BlueprintCallable)
    bool PreviousSaveIsAuto();

    UFUNCTION(BlueprintCallable)
    void LoadWorldTransitSave();

    UFUNCTION(BlueprintCallable)
    UFNAFSaveData* LoadSaveSlotData(const FString& SlotName);

    UFUNCTION(BlueprintCallable)
    void LoadMasterSave();

    UFUNCTION(BlueprintCallable)
    UFNAFSaveData* LoadChowdaAutoSaveData(int32 ProfileIndexIn);

    UFUNCTION(BlueprintCallable)
    void LoadChowdaAutoSave(int32 ProfileIndexIn);

    UFUNCTION(BlueprintCallable)
    void LoadCheckpoint();

    UFUNCTION(BlueprintCallable)
    void LoadAutoSave();

    UFUNCTION(BlueprintCallable)
    void LoadActivitySave(const FString& InActivityId);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsSavingAllowed() const;

    UFUNCTION(BlueprintCallable)
    bool IsInChapterReplay();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool HasSaveData() const;

    UFUNCTION(BlueprintCallable)
    bool HasPreviousSave();

    UFUNCTION(BlueprintCallable)
    bool HasChowdaSaveGame(int32 ProfileIndexIn);

    UFUNCTION(BlueprintCallable)
    bool HasAutoSave();

    UFUNCTION(BlueprintCallable)
    bool HasActivitySave(const FString& InActivityId);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    int32 GetUserIndex();

    UFUNCTION(BlueprintCallable)
    bool GetUseAutosaveMapArea();

    UFUNCTION(BlueprintCallable)
    EMapArea GetMapAreaToLoad();

    UFUNCTION(BlueprintCallable)
    bool GetLoadIntoChapter();

    UFUNCTION(BlueprintCallable)
    EMapArea GetLastSavedMapArea();

    UFUNCTION(BlueprintCallable)
    int32 GetLastSavedChapter();

    UFUNCTION(BlueprintCallable)
    FString GetLastLoadedChowdaSlotName();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    int32 GetGameIteration() const;

    UFUNCTION(BlueprintCallable)
    int32 GetCurrentChapter();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    int32 GetChowdaProfileIndex();

    UFUNCTION(BlueprintCallable)
    void Get_SaveSlotNameData(TMap<FString, FString>& SaveSlots_Map);

    UFUNCTION(BlueprintCallable)
    void Get_ChowdaSaveSlotNameData(TMap<FString, FString>& SaveSlots_Map);

    UFUNCTION(BlueprintCallable)
    void FinalizeCheckpoint();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool DLCCallToActionVisibility();

    UFUNCTION(BlueprintCallable)
    void DeleteSaveGameByName(const FString& SlotName);

    UFUNCTION(BlueprintCallable)
    void DeleteSavedGame_BySlot(int32 saveSlotNumber);

    UFUNCTION(BlueprintCallable)
    void DeleteChowdaSaveGameByName(const FString& SlotName);

    UFUNCTION(BlueprintCallable)
    void CreateWorldTransitSave();

    UFUNCTION(BlueprintCallable)
    void CreatePotentialCheckpoint();

    UFUNCTION(BlueprintCallable)
    void ClearChowdaAutoSaves();

    UFUNCTION(BlueprintCallable)
    void ChowdaAutoSave(int32 Chapter, EMapArea MapAreaToSave, FName SaveID);

    UFUNCTION(BlueprintCallable)
    void AutoSave();

    UFUNCTION(BlueprintCallable)
    void AsyncSaveGame(const FString& originalSlotName, bool isANewSlot, bool isCurrentSlot, const FString& Renamed_SaveSlot, FString& internal_SlotName);

    UFUNCTION(BlueprintCallable)
    void AsyncLoadGameLastLoaded();

    UFUNCTION(BlueprintCallable)
    void AsyncLoadGame(const FString& originalSlotName, bool isContinueSelected, FString& internal_SlotName);

    UFUNCTION(BlueprintCallable)
    void AsyncLoadChowdaLastLoaded();

private:
    bool GetHasSaveFromMapping(const FString& SlotName);
    void UpdateHasSaveMapping(const FString& SlotName, bool HasSave);
    void LoadGameInternal(const FString& InSaveSlotName);
    void CollectSaveData(UFNAFSaveData* SaveData);
    int32 GetSaveSlotNum(const FString& SlotName);
    int32 GetNextAvailableSaveSlotIndex();
    void OnGameSaveCompleteHandler(const FString& SlotName, const int32 PlayerIndex, bool bSuccess);
    void OnChowdaMasterSaveCompleteHandler(const FString& SlotName, const int32 PlayerIndex, bool bSuccess);
    void OnGameLoadCompleteHandler(const FString& SlotName, const int32 PlayerIndex, USaveGame* LoadedSaveGame);
};
