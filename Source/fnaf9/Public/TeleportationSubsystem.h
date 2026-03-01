#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NormalGlichSwapDelegate.h"
#include "SaveHandlerInterface.h"
#include "SwapFailedDelegate.h"
#include "Templates/SubclassOf.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "TeleportationSubsystem.generated.h"

class ANavigationData;
class APawn;
class ACharacter;
class UNavigationQueryFilter;
class UEnvQuery;
class UFNAFSaveData;

UCLASS(Blueprintable)
class FNAF9_API UTeleportationSubsystem : public UWorldSubsystem, public ISaveHandlerInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bIsTeleporting;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bDisableRestrictions;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bInATeleportZone;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    ANavigationData* NavData;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TSubclassOf<UNavigationQueryFilter> FilterClass;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FNormalGlichSwap NormalGlitchSwapDelegate;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FSwapFailed SwapFailedDelegate;

private:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bPlayerInNormal;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bPlayerSaveInNormal;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bAIInNormal;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float TeleportDistance;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float CoolDownTime;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float AICapsule;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float CassieCapsule;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool IsCoolDownComplete;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bNavMeshBlocked;

    UPROPERTY(Transient)
    UEnvQuery* TeleportationQueryNormal;

    UPROPERTY(Transient)
    UEnvQuery* TeleportationQueryAR;

    UPROPERTY(Transient)
    UEnvQuery* TeleportationQueryNormalAI;

    UPROPERTY(Transient)
    UEnvQuery* TeleportationQueryARAI;

    FTimerHandle EQSFallbackTimerHandle;

    UPROPERTY(Transient)
    APawn* CurrentAIPawn;

    float CurrentAICapsuleHeight;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    float MAX_EQS_TIMER;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FVector QueryExtent;

    FCriticalSection AIEQSGuard;

    bool bPlayerInNormalForChapterReplay;

public:
    UTeleportationSubsystem();

    UFUNCTION(BlueprintCallable)
    void TeleportAI(APawn* AIPawn, float CapsuleHeight);

    UFUNCTION(BlueprintCallable)
    void SetPlayerSaveInNormal(bool PlayerSaveInNormal);

    UFUNCTION(BlueprintCallable)
    void SetPlayerInNormalForChapterReplay(bool PlayerInNormal);

    UFUNCTION(BlueprintCallable)
    void SetPlayerInNormal(bool PlayerInNormal);

    UFUNCTION(BlueprintCallable)
    bool IsPlayerInNormal();

    UFUNCTION(BlueprintCallable)
    float GetTeleportationDistance();

    UFUNCTION(BlueprintCallable)
    bool GetPlayerSaveInNormal();

    UFUNCTION(BlueprintCallable)
    APawn* GetCurrentAIPawn();

    UFUNCTION(BlueprintCallable)
    void ForceTeleport();

    UFUNCTION(BlueprintCallable)
    bool CheckIfPlayerCanTeleport();

    UFUNCTION(BlueprintCallable)
    bool CanTeleport();

    // ISaveHandlerInterface
    virtual void OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject) override;
    virtual void OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject) override;

private:
    // Internal teleport pipeline
    void TeleportPlayer();
    void TeleportFXTimer();
    void TeleportCooldownTimer();
    void TeleportFinishHelper(bool bNewPlayerInNormal, UWorld* World, ACharacter* PlayerCharacter);

    // EQS query completion callbacks (player)
    void TeleportQueryFinishNormal(TSharedPtr<FEnvQueryResult> Result);
    void TeleportQueryFinishAR(TSharedPtr<FEnvQueryResult> Result);

    // EQS query completion callbacks (AI)
    void TeleportQueryFinishNormalAI(TSharedPtr<FEnvQueryResult> Result);
    void TeleportQueryFinishARAI(TSharedPtr<FEnvQueryResult> Result);

    // Overload: direct AI teleport finish with explicit pawn (when EQS skipped)
    void TeleportQueryFinishNormalAI(TSharedPtr<FEnvQueryResult> Result, APawn* AIPawn, float CapsuleHeight);

    // EQS timeout fallbacks
    void OnQueryTimeElapsedNormal();
    void OnQueryTimeElapsedAR();

    // Helpers
    ACharacter* GetPlayerCharacterChecked() const;
    UEnvQuery* LoadEQSQuery(UEnvQuery*& CachedQuery, const TCHAR* AssetPath);
};