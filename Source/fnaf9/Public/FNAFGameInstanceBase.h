#pragma once
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "EFNAFGameType.h"
#include "LatentActions.h"
#include "FNAFGameInstanceBase.generated.h"

class UFNAFGameUserSettings;
class UGameClockSystem;
class UFNAFSaveGameSystem;
class UFNAFInventorySystem;
class UFNAFMasterData;

/**
 * Custom latent action for async DLC content scan.
 * Runs an async task and completes when scanning finishes.
 */
class FChowdaScanLatentAction : public FPendingLatentAction
{
public:
    FChowdaScanLatentAction(const FLatentActionInfo& InLatentInfo, bool* InOutFinished)
        : ExecutionFunction(InLatentInfo.ExecutionFunction)
        , OutputLink(InLatentInfo.Linkage)
        , CallbackTarget(InLatentInfo.CallbackTarget)
        , bOutFinished(InOutFinished)
        , bScanComplete(false)
    {
    }

    void Start()
    {
        Future = Async(EAsyncExecution::ThreadPool, [this]()
            {
                // DLC content scan work happens here
                bScanComplete = true;
            });
    }

    virtual void UpdateOperation(FLatentResponse& Response) override
    {
        if (bScanComplete)
        {
            if (bOutFinished)
            {
                *bOutFinished = true;
            }
            Response.FinishAndTriggerIf(true, ExecutionFunction, OutputLink, CallbackTarget);
        }
    }

#if WITH_EDITOR
    virtual FString GetDescription() const override
    {
        return TEXT("FChowdaScanLatentAction");
    }
#endif

private:
    FName ExecutionFunction;
    int32 OutputLink;
    FWeakObjectPtr CallbackTarget;
    bool* bOutFinished;
    bool bScanComplete;
    TFuture<void> Future;
};

UCLASS(Blueprintable, NonTransient)
class FNAF9_API UFNAFGameInstanceBase : public UGameInstance {
    GENERATED_BODY()
public:
    UFNAFGameInstanceBase();

    virtual void Init() override;
    virtual void OnStart() override;

    UFUNCTION(BlueprintCallable)
    void StartGamePlay(EFNAFGameType GameType);

    UFUNCTION(BlueprintCallable)
    void SetVisualQualityLevel(int32 Level);

    UFUNCTION(BlueprintCallable)
    void SetSplashFinished(bool bFinished);

    UFUNCTION(BlueprintCallable)
    void SetRayTraceQualityLevel(int32 Level);

    UFUNCTION(BlueprintCallable)
    void SetPresenceForLocalPlayers(const FString& StatusStr, const FString& PresenceData);

    UFUNCTION(BlueprintCallable)
    void SetIsOnLoadingScreen(bool bOnLoadingScreen);

    UFUNCTION(BlueprintCallable)
    void SetIsFromTitleForChowda(bool In_FromTitle);

    UFUNCTION(BlueprintCallable)
    void SetAllPlayersFocusToViewport();

    UFUNCTION(BlueprintCallable)
    void ProcessActivityIntent();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnPlayerLoginChanged(bool bLoggedIn, int32 UserId);

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnPlayerControllerPairingChanged();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnLoadingScreenStart();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnLoadingScreenEnd();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnControllerConnectionChanged();

    UFUNCTION(BlueprintCallable)
    void OnGameActivityLoadComplete();

    UFUNCTION(BlueprintCallable, Exec)
    void LogGameClockDelegates();

    UFUNCTION(BlueprintCallable)
    void LoadGameTips(EFNAFGameType GameType);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsOnLoadingScreen() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsNormalPlay() const;

    UFUNCTION(BlueprintCallable)
    bool IsLoadingActivity();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsFromTitle() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool HasSplashFinished() const;

    UFUNCTION(BlueprintCallable)
    int32 GetVisualQualityLevel();

    UFUNCTION(BlueprintCallable)
    int32 GetRayTraceQualityLevel();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    FString GetPlayerName();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    int32 GetPlayerControllerID();

    UFUNCTION(BlueprintCallable)
    bool GetIsShippingConfig();

    UFUNCTION(BlueprintCallable)
    float GetGPUBenchmarkResult();

    UFUNCTION(BlueprintCallable)
    FText GetGameTipTextByIndexDLC(int32 Index);

    UFUNCTION(BlueprintCallable)
    FText GetGameTipTextByIndex(int32 Index);

    UFUNCTION(BlueprintCallable, BlueprintPure)
    EFNAFGameType GetCurrentGameType() const;

    UFUNCTION(BlueprintCallable)
    float GetCPUBenchmarkResult();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    FString GetAllLoadedLevelsString() const;

    UFUNCTION(BlueprintCallable)
    void EndLoadingScreenDLC();

    UFUNCTION(BlueprintCallable)
    void EndLoadingScreen();

    UFUNCTION(BlueprintCallable)
    void CheckForPlayerLoginChanged();

    UFUNCTION(BlueprintCallable)
    void ChangeLocalPlayerController(int32 UserId);

    UFUNCTION(BlueprintCallable)
    void BeginLoadingScreenDLC();

    UFUNCTION(BlueprintCallable)
    void BeginLoadingScreen();

    UFUNCTION(BlueprintCallable, meta = (Latent, LatentInfo = "LatentActionInfo"))
    void AsyncChowdaScan(bool bUseless, bool& bOutFinished, FLatentActionInfo LatentActionInfo);

    UFUNCTION(BlueprintCallable)
    bool IsSurvivalMode() const
    {
        return false; // placeholder
	}

protected:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EFNAFGameType CurrentGameType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bFromTitle;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bIsOnLoadingScreen;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bLoadingWidgetAdded;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bIsInBackground;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bGamePausedBeforeDeactivate;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bHasFinishedSplash;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bLoadingActivity;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    int32 PlayerGamepadId;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FString ActivityIdToLoad;

    UPROPERTY(Transient)
    TArray<FText> GameTips;

    UPROPERTY(Transient)
    TArray<FText> GameTips_DLC;

    // Not UPROPERTY — UHT disallows TWeakObjectPtr<UClass> ("class variables cannot be weak")
    TWeakObjectPtr<UClass> LoadingWidgetClass;
    TWeakObjectPtr<UClass> LoadingWidgetClassDLC;

    UPROPERTY(Transient)
    TWeakObjectPtr<UUserWidget> InstancedLoadingWidget;

    UPROPERTY(Transient)
    TWeakObjectPtr<UUserWidget> InstancedLoadingWidgetDLC;

    FDelegateHandle PostLoadDelegateHandle;

private:
    void OnApplicationDeactivated();
    void OnApplicationReactivated();

    void HandleControllerConnectionChange(bool bIsConnection, int32 UserId, int32 GamepadId);
    void HandleUserLoginCompleted(int32 LocalUserNum, bool bWasSuccessful, const FUniqueNetId& UserId, const FString& Error);
    void HandleUserLogoutCompleted(int32 LocalUserNum, bool bWasSuccessful);
    void HandlecontrollerPairingChange(int32 GameUserIndex, int32 NewUserPlatformId, int32 OldUserPlatformId);

    static bool CachedHasExpectedClassPassed;
    static FString DLC_ASSET_CHECK;
};