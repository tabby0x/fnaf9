#include "FNAFGameInstanceBase.h"
#include "FNAFGameUserSettings.h"
#include "GameClockSystem.h"
#include "FNAFSaveGameSystem.h"
#include "FNAFInventorySystem.h"
#include "FNAFMasterData.h"
#include "DownloadableContentHandler.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelStreaming.h"
#include "GameFramework/GameModeBase.h"
#include "Framework/Application/SlateApplication.h"
#include "Blueprint/UserWidget.h"
// TODO: Add "MoviePlayer" module to Build.cs and uncomment:
// #include "MoviePlayer.h"
// TODO: Add "AkAudio" module to Build.cs and uncomment:
// #include "AkAudioDevice.h"

DEFINE_LOG_CATEGORY_STATIC(LogFNAFGameInstance, Log, All);

bool UFNAFGameInstanceBase::CachedHasExpectedClassPassed = false;
FString UFNAFGameInstanceBase::DLC_ASSET_CHECK;

UFNAFGameInstanceBase::UFNAFGameInstanceBase()
    : CurrentGameType(EFNAFGameType::Normal)
    , bFromTitle(false)
    , bIsOnLoadingScreen(false)
    , bLoadingWidgetAdded(false)
    , bIsInBackground(false)
    , bGamePausedBeforeDeactivate(false)
    , bHasFinishedSplash(false)
    , bLoadingActivity(false)
    , PlayerGamepadId(-1)
{
}

void UFNAFGameInstanceBase::Init()
{
    Super::Init();

    // Check if DLC game mode class can be loaded to determine if DLC content is mounted
    if (!CachedHasExpectedClassPassed)
    {
        const FString& AssetPath = DLC_ASSET_CHECK;
        CachedHasExpectedClassPassed = StaticLoadClass(AGameModeBase::StaticClass(), nullptr, *AssetPath) != nullptr;
    }
    // TODO: UDownloadableContentHandler needs static bool bDLCPackMounted added to its header
    // UDownloadableContentHandler::bDLCPackMounted = CachedHasExpectedClassPassed;

    CurrentGameType = EFNAFGameType::Normal;
    bFromTitle = false;

    // Online subsystem delegates (console-specific -> PS5 Game Activities, login/logout)
    // Dummied out for PC build. Original bound:
    //   OnGameActivityActivation to external UI change
    //   HandleUserLoginCompleted to identity login complete
    //   HandleUserLogoutCompleted to identity logout complete

    FCoreDelegates::OnControllerConnectionChange.AddUObject(this, &UFNAFGameInstanceBase::HandleControllerConnectionChange);
    FCoreDelegates::OnControllerPairingChange.AddUObject(this, &UFNAFGameInstanceBase::HandlecontrollerPairingChange);
}

void UFNAFGameInstanceBase::OnStart()
{
    Super::OnStart();

    FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &UFNAFGameInstanceBase::OnApplicationDeactivated);
    FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(this, &UFNAFGameInstanceBase::OnApplicationReactivated);
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UFNAFGameInstanceBase::OnApplicationDeactivated);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UFNAFGameInstanceBase::OnApplicationReactivated);
}

void UFNAFGameInstanceBase::StartGamePlay(EFNAFGameType GameType)
{
    CurrentGameType = GameType;
    bFromTitle = true;

    UGameClockSystem* ClockSystem = GetSubsystem<UGameClockSystem>();
    if (ClockSystem)
    {
        ClockSystem->SetupSystem(GameType);
    }

    // Touch inventory system to ensure initialization
    GetSubsystem<UFNAFInventorySystem>();
}

void UFNAFGameInstanceBase::BeginLoadingScreen()
{
    LoadGameTips(EFNAFGameType::Normal);

    if (!LoadingWidgetClass.IsValid())
    {
        UClass* WidgetClass = StaticLoadClass(UUserWidget::StaticClass(), nullptr, TEXT("/Game/Blueprints/UI/Loading/LoadingGraphic.LoadingGraphic_C"));
        LoadingWidgetClass = WidgetClass;
    }

    if (LoadingWidgetClass.IsValid())
    {
        bIsOnLoadingScreen = true;
        OnLoadingScreenStart();

        if (!InstancedLoadingWidget.IsValid())
        {
            UUserWidget* Widget = CreateWidget<UUserWidget>(this, LoadingWidgetClass.Get());
            InstancedLoadingWidget = Widget;
            if (InstancedLoadingWidget.IsValid())
            {
                InstancedLoadingWidget->AddToRoot();
            }
        }

        UWorld* World = GetWorld();
        UGameViewportClient* GameViewport = World ? World->GetGameViewport() : nullptr;
        if (GameViewport && InstancedLoadingWidget.IsValid())
        {
            TSharedRef<SWidget> SlateWidget = InstancedLoadingWidget->TakeWidget();
            if (!bLoadingWidgetAdded)
            {
                bLoadingWidgetAdded = true;
                GameViewport->AddViewportWidgetContent(SlateWidget, 1024);
            }
        }

        // TODO: Wwise integration -> requires AkAudio module
        // FAkAudioDevice* AudioDevice = FAkAudioDevice::Get();
        // if (AudioDevice)
        // {
        //     AudioDevice->Suspend(false);
        // }
    }
}

void UFNAFGameInstanceBase::BeginLoadingScreenDLC()
{
    LoadGameTips(EFNAFGameType::ChowdaMode);

    UE_LOG(LogFNAFGameInstance, Log, TEXT("BeginLoadingScreenDLC"));

    if (!LoadingWidgetClassDLC.IsValid())
    {
        UClass* WidgetClass = StaticLoadClass(UUserWidget::StaticClass(), nullptr, TEXT("/Game/UI_DLC/DLC_LoadingGraphic.DLC_LoadingGraphic_C"));
        LoadingWidgetClassDLC = WidgetClass;
    }

    if (LoadingWidgetClassDLC.IsValid())
    {
        bIsOnLoadingScreen = true;

        if (!InstancedLoadingWidgetDLC.IsValid())
        {
            UUserWidget* Widget = CreateWidget<UUserWidget>(this, LoadingWidgetClassDLC.Get());
            InstancedLoadingWidgetDLC = Widget;
            if (InstancedLoadingWidgetDLC.IsValid())
            {
                InstancedLoadingWidgetDLC->AddToRoot();
                InstancedLoadingWidgetDLC->SetVisibility(ESlateVisibility::Visible);
                InstancedLoadingWidgetDLC->SetRenderOpacity(1.0f);
            }
        }

        UWorld* World = GetWorld();
        UGameViewportClient* GameViewport = World ? World->GetGameViewport() : nullptr;
        if (GameViewport && InstancedLoadingWidgetDLC.IsValid())
        {
            TSharedRef<SWidget> SlateWidget = InstancedLoadingWidgetDLC->TakeWidget();
            if (!bLoadingWidgetAdded)
            {
                bLoadingWidgetAdded = true;
                GameViewport->AddViewportWidgetContent(SlateWidget, 1024);
            }
        }

        // TODO: Wwise integration -> FAkAudioDevice::Get()->Suspend(false);

        UE_LOG(LogFNAFGameInstance, Log, TEXT("EndOfBeginLoadingScreen"));
    }
}

void UFNAFGameInstanceBase::EndLoadingScreen()
{
    // TODO: Add MoviePlayer module to Build.cs for GetMoviePlayer()
    // IGameMoviePlayer* MoviePlayer = GetMoviePlayer();
    // if (MoviePlayer)
    // {
    //     MoviePlayer->StopMovie();
    // }

    UWorld* World = GetWorld();
    UGameViewportClient* GameViewport = World ? World->GetGameViewport() : nullptr;

    if (InstancedLoadingWidget.IsValid())
    {
        TSharedPtr<SWidget> CachedWidget = InstancedLoadingWidget->GetCachedWidget();
        if (CachedWidget.IsValid() && GameViewport)
        {
            GameViewport->RemoveViewportWidgetContent(CachedWidget.ToSharedRef());
        }

        InstancedLoadingWidget->RemoveFromRoot();
    }

    if (PostLoadDelegateHandle.IsValid())
    {
        FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadDelegateHandle);
        PostLoadDelegateHandle.Reset();
    }

    // TODO: Wwise integration -> FAkAudioDevice::Get()->WakeupFromSuspend();

    bLoadingWidgetAdded = false;
    bIsOnLoadingScreen = false;
    OnLoadingScreenEnd();
}

void UFNAFGameInstanceBase::EndLoadingScreenDLC()
{
    UE_LOG(LogFNAFGameInstance, Log, TEXT("EndLoadingScreenDLC"));

    UWorld* World = GetWorld();
    UGameViewportClient* GameViewport = World ? World->GetGameViewport() : nullptr;

    if (InstancedLoadingWidgetDLC.IsValid())
    {
        TSharedPtr<SWidget> CachedWidget = InstancedLoadingWidgetDLC->GetCachedWidget();
        if (CachedWidget.IsValid() && GameViewport)
        {
            GameViewport->RemoveViewportWidgetContent(CachedWidget.ToSharedRef());
        }

        InstancedLoadingWidgetDLC->RemoveFromRoot();
    }

    if (PostLoadDelegateHandle.IsValid())
    {
        FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadDelegateHandle);
        PostLoadDelegateHandle.Reset();
    }

    // TODO: Wwise integration -> FAkAudioDevice::Get()->WakeupFromSuspend();

    bLoadingWidgetAdded = false;
    bIsOnLoadingScreen = false;

    UE_LOG(LogFNAFGameInstance, Log, TEXT("EndLoadingScreenDLC complete"));
}

void UFNAFGameInstanceBase::LoadGameTips(EFNAFGameType GameType)
{
    if (GameType == EFNAFGameType::ChowdaMode)
    {
        for (int32 i = 0; i < 4; i++)
        {
            FName TableName = FName(TEXT("/Game/Data/Localizations/Loc_GameTipsDLC.Loc_GameTipsDLC"));
            FString Key = FString::Printf(TEXT("Loc_GameTipsDLC_%d"), i);
            FText TipText = FText::FromStringTable(TableName, Key);
            GameTips_DLC.Add(TipText);
        }
    }
    else
    {
        for (int32 i = 0; i < 9; i++)
        {
            FName TableName = FName(TEXT("/Game/Data/Localizations/Base_Game_Loc/Loc_GameTips.Loc_GameTips"));
            FString Key = FString::Printf(TEXT("Loc_GameTips_%d"), i);
            FText TipText = FText::FromStringTable(TableName, Key);
            GameTips.Add(TipText);
        }
    }
}

FText UFNAFGameInstanceBase::GetGameTipTextByIndex(int32 Index)
{
    if (GameTips.Num() <= 0)
    {
        return FText::GetEmpty();
    }
    return GameTips[Index];
}

FText UFNAFGameInstanceBase::GetGameTipTextByIndexDLC(int32 Index)
{
    if (GameTips_DLC.Num() <= 0)
    {
        return FText::GetEmpty();
    }
    return GameTips_DLC[Index];
}

FString UFNAFGameInstanceBase::GetAllLoadedLevelsString() const
{
    FString Result;
    UWorld* World = GetWorld();
    if (!World)
    {
        return Result;
    }

    for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        FString StatusString;

        bool bShouldBeLoaded = StreamingLevel->ShouldBeLoaded();
        ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();

        if (bShouldBeLoaded)
        {
            if (LoadedLevel)
            {
                if (LoadedLevel->bIsVisible)
                {
                    StatusString = TEXT("Visible");
                }
                else
                {
                    StatusString = TEXT("Loaded");
                }
            }
            else
            {
                StatusString = TEXT("Loading");
            }
        }
        else
        {
            if (LoadedLevel)
            {
                StatusString = TEXT("Unloading");
            }
            else
            {
                continue;
            }
        }

        FString LevelName = StreamingLevel->GetWorldAssetPackageName();

        // Strip path prefix -> keep only the level name after the last '/'
        int32 LastSlash;
        if (LevelName.FindLastChar(TEXT('/'), LastSlash))
        {
            LevelName.MidInline(LastSlash, LevelName.Len() - LastSlash, false);
        }

        Result += FString::Printf(TEXT("<%s>%s: %s</>\n"), *StatusString, *StatusString, *LevelName);
    }

    return Result;
}

int32 UFNAFGameInstanceBase::GetVisualQualityLevel()
{
    UFNAFGameUserSettings* Settings = Cast<UFNAFGameUserSettings>(GEngine->GetGameUserSettings());
    if (Settings)
    {
        return Settings->VisualQualityLevel;
    }
    return 0;
}

int32 UFNAFGameInstanceBase::GetRayTraceQualityLevel()
{
    UFNAFGameUserSettings* Settings = Cast<UFNAFGameUserSettings>(GEngine->GetGameUserSettings());
    if (Settings)
    {
        return Settings->RayTraceQualityLevel;
    }
    return 0;
}

void UFNAFGameInstanceBase::SetVisualQualityLevel(int32 Level)
{
    if (Level > 2)
    {
        return;
    }

    UFNAFGameUserSettings* Settings = Cast<UFNAFGameUserSettings>(GEngine->GetGameUserSettings());
    if (Settings)
    {
        Settings->VisualQualityLevel = Level;
    }
}

void UFNAFGameInstanceBase::SetRayTraceQualityLevel(int32 Level)
{
    if (Level > 2)
    {
        return;
    }

    UFNAFGameUserSettings* Settings = Cast<UFNAFGameUserSettings>(GEngine->GetGameUserSettings());
    if (Settings)
    {
        Settings->RayTraceQualityLevel = Level;
    }
}

FString UFNAFGameInstanceBase::GetPlayerName()
{
    if (LocalPlayers.Num() > 0)
    {
        ULocalPlayer* LP = LocalPlayers[0];
        if (LP)
        {
            return LP->GetNickname();
        }
    }
    return FString();
}

void UFNAFGameInstanceBase::HandleControllerConnectionChange(bool bIsConnection, int32 UserId, int32 GamepadId)
{
    APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
    if (PC)
    {
        PC->GetLocalPlayer();
    }

    if (!bIsConnection)
    {
        GamepadId = -1;
    }
    PlayerGamepadId = GamepadId;
    OnControllerConnectionChanged();
}

void UFNAFGameInstanceBase::HandleUserLoginCompleted(int32 LocalUserNum, bool bWasSuccessful, const FUniqueNetId& UserId, const FString& Error)
{
    if (bWasSuccessful)
    {
        APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
        if (PC)
        {
            ULocalPlayer* LP = PC->GetLocalPlayer();
            if (LP)
            {
                if (PlayerGamepadId > -1)
                {
                    LP->SetControllerId(PlayerGamepadId);
                }
            }
        }
        OnPlayerLoginChanged(true, LocalUserNum);
    }
}

void UFNAFGameInstanceBase::HandleUserLogoutCompleted(int32 LocalUserNum, bool bWasSuccessful)
{
    if (bWasSuccessful)
    {
        APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
        if (PC)
        {
            ULocalPlayer* LP = PC->GetLocalPlayer();
            if (LP)
            {
                PlayerGamepadId = LP->GetControllerId();
            }
        }
        OnPlayerLoginChanged(false, LocalUserNum);
    }
}

void UFNAFGameInstanceBase::HandlecontrollerPairingChange(int32 GameUserIndex, int32 NewUserPlatformId, int32 OldUserPlatformId)
{
    OnPlayerControllerPairingChanged();
}

void UFNAFGameInstanceBase::OnApplicationDeactivated()
{
    if (!bIsInBackground)
    {
        bIsInBackground = true;
        bGamePausedBeforeDeactivate = UGameplayStatics::IsGamePaused(this);
        UGameplayStatics::SetGamePaused(this, true);

        // TODO: Wwise integration -> FAkAudioDevice::Get()->Suspend(false);
    }
}

void UFNAFGameInstanceBase::OnApplicationReactivated()
{
    if (bIsInBackground)
    {
        bIsInBackground = false;
        if (!bGamePausedBeforeDeactivate)
        {
            UGameplayStatics::SetGamePaused(this, false);
        }

        // TODO: Wwise integration -> FAkAudioDevice::Get()->WakeupFromSuspend();
    }
}

void UFNAFGameInstanceBase::CheckForPlayerLoginChanged()
{
    APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
    if (PC)
    {
        PC->GetLocalPlayer();
    }
}

void UFNAFGameInstanceBase::ChangeLocalPlayerController(int32 UserIndex)
{
    if (LocalPlayers.Num() > 0)
    {
        ULocalPlayer* LP = LocalPlayers[0];
        if (LP)
        {
            // Original calls a vtable function on the local player (offset 688)
            // then notifies login changed
            OnPlayerLoginChanged(true, UserIndex);
        }
    }
}

void UFNAFGameInstanceBase::SetIsFromTitleForChowda(bool In_FromTitle)
{
    if (CurrentGameType == EFNAFGameType::ChowdaMode)
    {
        bFromTitle = In_FromTitle;
    }
}

void UFNAFGameInstanceBase::SetAllPlayersFocusToViewport()
{
    FSlateApplication::Get().SetAllUserFocusToGameViewport(EFocusCause::SetDirectly);
}

bool UFNAFGameInstanceBase::IsFromTitle() const
{
    return bFromTitle;
}

bool UFNAFGameInstanceBase::IsOnLoadingScreen() const
{
    return bIsOnLoadingScreen;
}

bool UFNAFGameInstanceBase::IsLoadingActivity()
{
    return bLoadingActivity;
}

bool UFNAFGameInstanceBase::IsNormalPlay() const
{
    return CurrentGameType == EFNAFGameType::Normal;
}

bool UFNAFGameInstanceBase::HasSplashFinished() const
{
    return bHasFinishedSplash;
}

EFNAFGameType UFNAFGameInstanceBase::GetCurrentGameType() const
{
    return CurrentGameType;
}

void UFNAFGameInstanceBase::SetSplashFinished(bool bFinished)
{
    bHasFinishedSplash = bFinished;
}

void UFNAFGameInstanceBase::SetIsOnLoadingScreen(bool bOnLoadingScreen)
{
    bIsOnLoadingScreen = bOnLoadingScreen;
}

int32 UFNAFGameInstanceBase::GetPlayerControllerID()
{
    return PlayerGamepadId;
}

bool UFNAFGameInstanceBase::GetIsShippingConfig()
{
#if UE_BUILD_SHIPPING
    return true;
#else
    return false;
#endif
}

float UFNAFGameInstanceBase::GetGPUBenchmarkResult()
{
    return 0.0f;
}

float UFNAFGameInstanceBase::GetCPUBenchmarkResult()
{
    return 0.0f;
}

void UFNAFGameInstanceBase::LogGameClockDelegates()
{
    UGameClockSystem* ClockSystem = GetSubsystem<UGameClockSystem>();
    if (ClockSystem)
    {
        ClockSystem->LogConnectedDelegates();
    }
}

void UFNAFGameInstanceBase::OnGameActivityLoadComplete()
{
    UFNAFSaveGameSystem* SaveSystem = GetSubsystem<UFNAFSaveGameSystem>();
    if (IsValid(SaveSystem))
    {
        // Unbind OnGameActivityLoadComplete from OnLoadGameComplete
        SaveSystem->OnLoadGameComplete.RemoveAll(this);
    }

    CurrentGameType = EFNAFGameType::Normal;
    bFromTitle = true;

    UGameClockSystem* ClockSystem = GetSubsystem<UGameClockSystem>();
    if (ClockSystem)
    {
        ClockSystem->SetupSystem(EFNAFGameType::Normal);
    }

    // Touch inventory system to ensure initialization
    GetSubsystem<UFNAFInventorySystem>();

    UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/Maps/World/MAP_TheWorld")), true);
    bLoadingActivity = false;
}

void UFNAFGameInstanceBase::ProcessActivityIntent()
{
    // PS5 Game Activity processing -> dummied for PC
    // Original: binds OnGameActivityLoadComplete to SaveSystem->OnLoadGameComplete,
    // looks up ActivityIdToLoad in MasterData->ActivityIdSaveSlotNamesMap,
    // and calls SaveSystem->LoadGameInternal() with the mapped slot name.
    bFromTitle = true;
}

void UFNAFGameInstanceBase::SetPresenceForLocalPlayers(const FString& StatusStr, const FString& PresenceData)
{
    // Online presence -> requires OnlineSubsystem module (console feature)
    // Original: iterates LocalPlayers, gets each player's UniqueNetId,
    // builds FOnlineUserPresenceStatus with StatusStr and PresenceData,
    // calls IOnlinePresence::SetPresence()
}

void UFNAFGameInstanceBase::AsyncChowdaScan(bool bUseless, bool& bOutFinished, FLatentActionInfo LatentActionInfo)
{
    bOutFinished = false;

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FLatentActionManager& LatentManager = World->GetLatentActionManager();
    if (LatentManager.FindExistingAction<FChowdaScanLatentAction>(LatentActionInfo.CallbackTarget, LatentActionInfo.UUID) == nullptr)
    {
        FChowdaScanLatentAction* Action = new FChowdaScanLatentAction(LatentActionInfo, &bOutFinished);
        Action->Start();
        LatentManager.AddNewAction(LatentActionInfo.CallbackTarget, LatentActionInfo.UUID, Action);
    }
}