#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "EFNAFGameType.h"
#include "OnGameClockStateChangeDelegate.h"
#include "OnGameClockTickDelegate.h"
#include "OnGameClockTickDeltaDelegate.h"
#include "OnGameClockTimeChangedDelegate.h"
#include "OnTimeReachedDynamicDelegateDelegate.h"
#include "SaveHandlerInterface.h"
#include "GameClockSystem.generated.h"

class UFNAFSaveData;

// Time-triggered delegate entry (28 bytes per IDA: Hour, Minute, Delegate, bFired)
USTRUCT()
struct FGameClockTimeDelegate
{
    GENERATED_BODY()

    int32 Hour;
    int32 Minute;
    FOnTimeReachedDynamicDelegate Delegate;
    bool bFired;

    FGameClockTimeDelegate()
        : Hour(0), Minute(0), bFired(false)
    {
    }

    FGameClockTimeDelegate(int32 InHour, int32 InMinute, const FOnTimeReachedDynamicDelegate& InDelegate)
        : Hour(InHour), Minute(InMinute), Delegate(InDelegate), bFired(false)
    {
    }
};

UCLASS(Blueprintable)
class FNAF9_API UGameClockSystem : public UGameInstanceSubsystem, public ISaveHandlerInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnGameClockStateChange OnGameClockStateChange;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnTimeReachedDynamicDelegate NormalModeDelegate;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnTimeReachedDynamicDelegate MoonmanLiteDelegate;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnTimeReachedDynamicDelegate MoonmanIntermediateDelegate;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnTimeReachedDynamicDelegate MoonmanDangerDelegate;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnGameClockTick OnGameClockTick;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnGameClockTimeChanged OnGameClockTimeChanged;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnGameClockTickDelta OnGameClockTickDelta;

    UGameClockSystem();

    // ISaveHandlerInterface
    virtual void OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject) override;
    virtual void OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject) override;
    virtual void OnCheckpointSave_Implementation(UFNAFSaveData* SaveDataObject) override;
    virtual void OnCheckpointLoad_Implementation(UFNAFSaveData* SaveDataObject) override;
    virtual void PostSaveGame_Implementation() override;
    virtual void PostGameLoad_Implementation() override;

    UFUNCTION(BlueprintCallable)
    void SetupSystem(EFNAFGameType GameType);

    UFUNCTION(BlueprintCallable)
    void StartNextHour();

    UFUNCTION(BlueprintCallable)
    void SetGameTimeEvent(FOnTimeReachedDynamicDelegate Delegate, int32 Hour, int32 Minute);

    UFUNCTION(BlueprintCallable)
    void SetGameEndEvent(FOnTimeReachedDynamicDelegate Delegate);

    UFUNCTION(BlueprintCallable)
    void SetCurrentTime(int32 Hour, int32 Minute, bool bPlayDelegates);

    UFUNCTION(BlueprintCallable)
    void SetCurrentMinute(int32 Minute);

    UFUNCTION(BlueprintCallable)
    void SetCurrentHour(int32 Hour);

    UFUNCTION(BlueprintCallable)
    void SetClockRunning(bool bRunClock);

    UFUNCTION(BlueprintCallable)
    void SetClockRateInMinutesPerHour(int32 MinutesPerHour);

    UFUNCTION(BlueprintCallable)
    void SetClockRate(int32 TotalRealHours);

    UFUNCTION(BlueprintCallable)
    void ResetTimeDelegatesUpTo(int32 Hour, int32 Minute);

private:
    UFUNCTION(BlueprintCallable)
    void OnNormalModeTriggered();

    UFUNCTION(BlueprintCallable)
    void OnMoonmanLiteTriggered();

    UFUNCTION(BlueprintCallable)
    void OnMoonmanIntermediateTriggered();

    UFUNCTION(BlueprintCallable)
    void OnMoonmanDangerTriggered();

    // Timer callback -> called every TickRate real seconds
    void GameClockTick();

    // Debug logging
    void LogConnectedDelegates();

    // Hidden members restored from IDA constructor (initialization order preserved)
    FTimerHandle TimerHandle;
    bool bIsClockRunning;
    float CurrentGameTime;            // In game seconds
    TArray<FGameClockTimeDelegate> TimeDelegates;
    TArray<FOnTimeReachedDynamicDelegate> GameEndDelegates;
    bool bEndGameReached;
    float TotalGameTimeSeconds;       // Total game duration in seconds (from settings)
    float TotalRealTimeSeconds;       // Total real duration in seconds
    float SecondsPerTick;             // Game seconds advanced per tick

    // IDA: initialized to -1 in constructor
    int32 ELEVEN_PM;

public:
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsClockRunning() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetCurrentTimeInSeconds() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    void GetCurrentTime(int32& Hour, int32& Minute) const;
};