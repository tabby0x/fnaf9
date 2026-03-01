/*
 * RUIN world-switching system. Normal and AR (glitch) worlds are stacked
 * vertically, TeleportDistance units apart (default 5000). Player teleports
 * between them via nav raycasts, with EQS fallback if direct nav fails.
 */

#include "TeleportationSubsystem.h"
#include "TeleportationInterface.h"
#include "AIManagementSystem.h"
#include "FNAFChowdaSaveData.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "Engine/World.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogTeleport, Warning, All);

UTeleportationSubsystem::UTeleportationSubsystem()
{
    bIsTeleporting = false;
    bDisableRestrictions = false;
    bInATeleportZone = true;
    NavData = nullptr;
    FilterClass = nullptr;
    bPlayerInNormal = true;
    bPlayerSaveInNormal = true;
    bAIInNormal = true;
    TeleportDistance = 5000.0f;
    CoolDownTime = 0.0f;
    AICapsule = 96.0f;
    CassieCapsule = 60.0f;
    IsCoolDownComplete = true;
    bNavMeshBlocked = false;

    TeleportationQueryNormal = nullptr;
    TeleportationQueryAR = nullptr;
    TeleportationQueryNormalAI = nullptr;
    TeleportationQueryARAI = nullptr;
    CurrentAIPawn = nullptr;
    CurrentAICapsuleHeight = 0.0f;
    MAX_EQS_TIMER = 5.0f;
    QueryExtent = FVector(500.0f, 500.0f, 500.0f);
    bPlayerInNormalForChapterReplay = true;
}

ACharacter* UTeleportationSubsystem::GetPlayerCharacterChecked() const
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;

    ACharacter* PC = UGameplayStatics::GetPlayerCharacter(World, 0);
    if (!PC || !PC->IsValidLowLevel()) return nullptr;

    return PC;
}

UEnvQuery* UTeleportationSubsystem::LoadEQSQuery(UEnvQuery*& CachedQuery, const TCHAR* AssetPath)
{
    if (!CachedQuery || !CachedQuery->IsValidLowLevel())
    {
        CachedQuery = Cast<UEnvQuery>(
            StaticLoadObject(UEnvQuery::StaticClass(), nullptr, AssetPath));
    }
    return CachedQuery;
}

bool UTeleportationSubsystem::IsPlayerInNormal()
{
    return bPlayerInNormal;
}

float UTeleportationSubsystem::GetTeleportationDistance()
{
    return TeleportDistance;
}

bool UTeleportationSubsystem::GetPlayerSaveInNormal()
{
    return bPlayerSaveInNormal;
}

APawn* UTeleportationSubsystem::GetCurrentAIPawn()
{
    return CurrentAIPawn;
}

void UTeleportationSubsystem::SetPlayerSaveInNormal(bool PlayerSaveInNormal)
{
    bPlayerSaveInNormal = PlayerSaveInNormal;
}

void UTeleportationSubsystem::SetPlayerInNormalForChapterReplay(bool PlayerInNormal)
{
    bPlayerInNormalForChapterReplay = PlayerInNormal;
}

//
//   1. Get player character, verify valid
//   2. If player implements ITeleportationInterface:
//      Check: !bDisableRestrictions AND (bIsTeleporting OR !bInATeleportZone
//              OR !IsCoolDownComplete OR IsPlayerMoving() OR CurrentAIPawn)
//      → any of those true = cannot teleport
//   3. If player does NOT implement interface:
//      Check: !bDisableRestrictions AND (bIsTeleporting OR !bInATeleportZone
//              OR !IsCoolDownComplete)
//   4. Otherwise: can teleport
bool UTeleportationSubsystem::CanTeleport()
{
    ACharacter* PlayerChar = GetPlayerCharacterChecked();
    if (!PlayerChar)
    {
        return false;
    }

    if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
    {
        if (!bDisableRestrictions
            && (bIsTeleporting
                || !bInATeleportZone
                || !IsCoolDownComplete
                || ITeleportationInterface::Execute_IsPlayerMoving(PlayerChar)
                || CurrentAIPawn))
        {
            return false;
        }
    }
    else
    {
        if (!bDisableRestrictions
            && (bIsTeleporting || !bInATeleportZone || !IsCoolDownComplete))
        {
            return false;
        }
    }

    return true;
}

//
//   - On failure: calls Execute_PlayerCannotTeleport, broadcasts SwapFailedDelegate
//   - On success: calls Execute_ActivateTeleportFX, starts TeleportFXTimer
bool UTeleportationSubsystem::CheckIfPlayerCanTeleport()
{
    ACharacter* PlayerChar = GetPlayerCharacterChecked();
    if (!PlayerChar)
    {
        return false;
    }

    bool bCanTeleport = true;

    if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
    {
        if (!bDisableRestrictions
            && (bIsTeleporting
                || !bInATeleportZone
                || !IsCoolDownComplete
                || ITeleportationInterface::Execute_IsPlayerMoving(PlayerChar)
                || CurrentAIPawn))
        {
            bCanTeleport = false;
        }
    }
    else
    {
        if (!bDisableRestrictions
            && (bIsTeleporting || !bInATeleportZone || !IsCoolDownComplete))
        {
            bCanTeleport = false;
        }
    }

    if (!bCanTeleport)
    {
        if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
        {
            ITeleportationInterface::Execute_PlayerCannotTeleport(PlayerChar);
        }

        if (SwapFailedDelegate.IsBound())
        {
            SwapFailedDelegate.Broadcast();
        }

        return false;
    }

    if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
    {
        ITeleportationInterface::Execute_ActivateTeleportFX(PlayerChar);
        TeleportFXTimer();
        return true;
    }

    return false;
}

//
//   - If currently teleporting: set 0.2s timer to retry (recursive)
//   - If NOT bPlayerInNormal: activate FX and start timer
//   - Otherwise: do nothing (already in normal, no forced teleport needed)
void UTeleportationSubsystem::ForceTeleport()
{
    ACharacter* PlayerChar = GetPlayerCharacterChecked();
    if (!PlayerChar)
    {
        return;
    }

    if (!PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
    {
        return;
    }

    if (bIsTeleporting)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            FTimerHandle RetryHandle;
            World->GetTimerManager().SetTimer(
                RetryHandle,
                this,
                &UTeleportationSubsystem::ForceTeleport,
                0.2f,
                false);
        }
        return;
    }

    if (!bPlayerInNormal)
    {
        ITeleportationInterface::Execute_ActivateTeleportFX(PlayerChar);
        TeleportFXTimer();
    }
}

//
//   1. Set bPlayerInNormal = a2
//   2. Get player character
//   3. If NOT bPlayerInNormal: call ActivateGlitchUI
//   4. If bPlayerInNormal: call DeactivateGlitchUI
//   5. Start cooldown timer
//   6. If NormalGlitchSwapDelegate bound: broadcast with bPlayerInNormal
void UTeleportationSubsystem::SetPlayerInNormal(bool PlayerInNormal)
{
    bPlayerInNormal = PlayerInNormal;

    ACharacter* PlayerChar = GetPlayerCharacterChecked();
    if (!PlayerChar)
    {
        return;
    }

    if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
    {
        if (!bPlayerInNormal)
        {
            ITeleportationInterface::Execute_ActivateGlitchUI(PlayerChar);
        }

        if (bPlayerInNormal)
        {
            ITeleportationInterface::Execute_DeactivateGlitchUI(PlayerChar);
        }
    }

    TeleportCooldownTimer();

    if (NormalGlitchSwapDelegate.IsBound())
    {
        NormalGlitchSwapDelegate.Broadcast(bPlayerInNormal);
    }
}

//
//   1. Set bIsTeleporting = true
//   2. Set IsCoolDownComplete = false
//   3. Set 1.0s timer bound to TeleportPlayer
//   4. If world invalid: reset both flags
void UTeleportationSubsystem::TeleportFXTimer()
{
    bIsTeleporting = true;
    IsCoolDownComplete = false;

    UWorld* World = GetWorld();
    if (!World)
    {
        bIsTeleporting = false;
        IsCoolDownComplete = true;
        return;
    }

    FTimerHandle TimerHandle;
    World->GetTimerManager().SetTimer(
        TimerHandle,
        this,
        &UTeleportationSubsystem::TeleportPlayer,
        1.0f,
        false);
}

//
//   - If CoolDownTime <= 0: immediately set IsCoolDownComplete = true
//   - Otherwise: set timer, IsCoolDownComplete set true via lambda on expiry
void UTeleportationSubsystem::TeleportCooldownTimer()
{
    if (CoolDownTime <= 0.0f)
    {
        IsCoolDownComplete = true;
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FTimerHandle TimerHandle;
    World->GetTimerManager().SetTimer(
        TimerHandle,
        [this]()
        {
            IsCoolDownComplete = true;
        },
        CoolDownTime,
        false);
}

//
// The core teleport logic. Two symmetric branches:
//
// If bPlayerInNormal (going Normal → AR):
//   - SpringArmDisabled + ActivateGlitchUI on player
//   - Offset Z by -TeleportDistance (going DOWN to AR layer)
//   - NavigationRaycast downward
//   - Success: teleport directly, TeleportFinishHelper(false)
//   - Fail: run EQS (EQWorldSwitchNormal), set fallback timer
//
// If !bPlayerInNormal (going AR → Normal):
//   - SpringArmDisabled + DeactivateGlitchUI on player
//   - Offset Z by +TeleportDistance (going UP to Normal layer)
//   - NavigationRaycast downward from elevated position
//   - Success: teleport directly, TeleportFinishHelper(true)
//   - Fail: run EQS (EQWorldSwitchAR), set fallback timer
void UTeleportationSubsystem::TeleportPlayer()
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        return;
    }

    ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
    APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);

    if (!PlayerChar || !PlayerChar->IsValidLowLevel()
        || !PC || !PC->IsValidLowLevel())
    {
        return;
    }

    FVector TeleportLocation = PlayerChar->GetActorLocation();
    FQuat TeleportRotation = PlayerChar->GetActorQuat();

    if (bPlayerInNormal)
    {
        // ---- Normal → AR: going DOWN ----
        UE_LOG(LogTeleport, Log, TEXT("TeleportPlayer(): Normal → AR branch"));

        if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
        {
            ITeleportationInterface::Execute_SpringArmDisabled(PlayerChar);
            ITeleportationInterface::Execute_ActivateGlitchUI(PlayerChar);
        }

        FVector RayStart = TeleportLocation;
        RayStart.Z -= TeleportDistance;
        FVector RayEnd = RayStart;
        RayEnd.Z -= 50.0f;

        FVector NavHitLocation;
        bNavMeshBlocked = UNavigationSystemV1::NavigationRaycast(
            World, RayStart, RayEnd, NavHitLocation, FilterClass, PC);

        if (!bNavMeshBlocked)
        {
            NavHitLocation.Z += CassieCapsule;
            UE_LOG(LogTeleport, Log, TEXT("TeleportPlayer(): Normal Branch Direct Nav Finished"));
            PlayerChar->SetActorLocationAndRotation(NavHitLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
            TeleportFinishHelper(false, World, PlayerChar);
            return;
        }

        // Direct nav failed — run EQS
        UEnvQuery* Query = LoadEQSQuery(TeleportationQueryNormal,
            TEXT("/Game/Blueprints/AI/EnvQueries/EQWorldSwitchNormal.EQWorldSwitchNormal"));

        if (!Query || !Query->IsValidLowLevel())
        {
            UE_LOG(LogTeleport, Error,
                TEXT("TeleportPlayer(): EQS File (/Game/Blueprints/AI/EnvQueries/EQWorldSwitchNormal.EQWorldSwitchNormal) was not loaded!"));
            TSharedPtr<FEnvQueryResult> EmptyResult;
            TeleportQueryFinishNormal(EmptyResult);
            return;
        }

        UE_LOG(LogTeleport, Log, TEXT("TeleportPlayer(): Normal Branch Direct Nav Failed, Beginning EQS"));

        FEnvQueryRequest QueryRequest(Query);
        QueryRequest.Execute(EEnvQueryRunMode::SingleResult,
            FQueryFinishedSignature::CreateUObject(this, &UTeleportationSubsystem::TeleportQueryFinishNormal));

        World->GetTimerManager().SetTimer(
            EQSFallbackTimerHandle,
            this,
            &UTeleportationSubsystem::OnQueryTimeElapsedNormal,
            MAX_EQS_TIMER,
            false);
    }
    else
    {
        // ---- AR → Normal: going UP ----
        UE_LOG(LogTeleport, Log, TEXT("TeleportPlayer(): AR → Normal branch"));

        if (PlayerChar->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
        {
            ITeleportationInterface::Execute_SpringArmDisabled(PlayerChar);
            ITeleportationInterface::Execute_DeactivateGlitchUI(PlayerChar);
        }

        FVector RayStart = TeleportLocation;
        RayStart.Z += TeleportDistance;
        FVector RayEnd = RayStart;
        RayEnd.Z -= 50.0f;

        FVector NavHitLocation;
        bNavMeshBlocked = UNavigationSystemV1::NavigationRaycast(
            World, RayStart, RayEnd, NavHitLocation, FilterClass, PC);

        if (!bNavMeshBlocked)
        {
            NavHitLocation.Z += CassieCapsule;
            UE_LOG(LogTeleport, Log, TEXT("TeleportPlayer(): AR Branch Direct Nav Finished"));
            PlayerChar->SetActorLocationAndRotation(NavHitLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
            TeleportFinishHelper(true, World, PlayerChar);
            return;
        }

        // Direct nav failed — run EQS
        UEnvQuery* Query = LoadEQSQuery(TeleportationQueryAR,
            TEXT("/Game/Blueprints/AI/EnvQueries/EQWorldSwitchAR.EQWorldSwitchAR"));

        if (!Query || !Query->IsValidLowLevel())
        {
            UE_LOG(LogTeleport, Error,
                TEXT("TeleportPlayer(): EQS File (/Game/Blueprints/AI/EnvQueries/EQWorldSwitchAR.EQWorldSwitchAR) was not loaded!"));
            TSharedPtr<FEnvQueryResult> EmptyResult;
            TeleportQueryFinishAR(EmptyResult);
            return;
        }

        UE_LOG(LogTeleport, Log, TEXT("TeleportPlayer(): AR Branch Direct Nav Failed, Beginning EQS"));

        FEnvQueryRequest QueryRequest(Query);
        QueryRequest.Execute(EEnvQueryRunMode::SingleResult,
            FQueryFinishedSignature::CreateUObject(this, &UTeleportationSubsystem::TeleportQueryFinishAR));

        World->GetTimerManager().SetTimer(
            EQSFallbackTimerHandle,
            this,
            &UTeleportationSubsystem::OnQueryTimeElapsedAR,
            MAX_EQS_TIMER,
            false);
    }
}

//
//   1. Set bPlayerInNormal = newState
//   2. Call Execute_PlayerFinishedTeleporting on player
//   3. Start cooldown timer
//   4. Broadcast NormalGlitchSwapDelegate
//   5. Call Execute_SpringArmEnabled on player
//   6. Get AIManagementSystem, iterate RegisteredPawns
//   7. For each pawn implementing ITeleportationInterface:
//      a. Lock AIEQSGuard
//      b. Check CanAITeleport()
//      c. Get AICapsuleHalfHeight()
//      d. Call TeleportAI(pawn, capsuleHeight)
//   8. Clear CurrentAIPawn, set bIsTeleporting = false
void UTeleportationSubsystem::TeleportFinishHelper(bool bNewPlayerInNormal, UWorld* World, ACharacter* PlayerCharacter)
{
    bPlayerInNormal = bNewPlayerInNormal;

    if (PlayerCharacter->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
    {
        ITeleportationInterface::Execute_PlayerFinishedTeleporting(PlayerCharacter);
    }

    TeleportCooldownTimer();

    if (NormalGlitchSwapDelegate.IsBound())
    {
        NormalGlitchSwapDelegate.Broadcast(bPlayerInNormal);
        ITeleportationInterface::Execute_SpringArmEnabled(PlayerCharacter);
    }

    UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
    if (AIMgr && AIMgr->IsValidLowLevel())
    {
        TArray<APawn*> Pawns;
        AIMgr->GetAllAnimatronicPawns(Pawns);

        for (int32 i = 0; i < Pawns.Num(); ++i)
        {
            APawn* AIPawn = Pawns[i];

            AIEQSGuard.Lock();

            if (!AIPawn || !AIPawn->GetClass()->ImplementsInterface(UTeleportationInterface::StaticClass()))
            {
                UE_LOG(LogTeleport, Warning, TEXT("There is no interface"));
                CurrentAIPawn = nullptr;
                AIEQSGuard.Unlock();
                continue;
            }

            UE_LOG(LogTeleport, Log, TEXT("Executing Interface"));

            if (!ITeleportationInterface::Execute_CanAITeleport(AIPawn))
            {
                UE_LOG(LogTeleport, Log, TEXT("Can Teleport Is False"));
                CurrentAIPawn = nullptr;
                AIEQSGuard.Unlock();
                continue;
            }

            float AIHeight = ITeleportationInterface::Execute_AICapsuleHalfHeight(AIPawn);
            AICapsule = AIHeight;
            CurrentAIPawn = AIPawn;
            CurrentAICapsuleHeight = AIHeight;
            TeleportAI(AIPawn, AIHeight);
        }

        CurrentAIPawn = nullptr;
        bIsTeleporting = false;
    }
    else
    {
        CurrentAIPawn = nullptr;
        bIsTeleporting = false;
    }
}

//
// Same pattern as TeleportPlayer but for AI pawns:
//   - If bPlayerInNormal (AI going Normal → AR):
//     Offset Z by +TeleportDistance, raycast, set bAIInNormal=true on success
//   - If !bPlayerInNormal (AI going AR → Normal):
//     Offset Z by -TeleportDistance, raycast, set bAIInNormal=false on success
//   - On nav failure: run EQS with AI-specific queries
//
// NOTE: AI direction is OPPOSITE to player:
//   Player goes Normal→AR (down), AI follows to match player's new world
//   So AI TeleportDistance is applied UPWARD when !bPlayerInNormal
void UTeleportationSubsystem::TeleportAI(APawn* AIPawn, float CapsuleHeight)
{
    if (!AIPawn || !AIPawn->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("The AIPawn is invalid"));
        CurrentAIPawn = nullptr;
        AIEQSGuard.Unlock();
        return;
    }

    AController* AIController = AIPawn->Controller;
    if (!AIController || !AIController->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("The AIController is invalid"));
        CurrentAIPawn = nullptr;
        AIEQSGuard.Unlock();
        return;
    }

    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        CurrentAIPawn = nullptr;
        AIEQSGuard.Unlock();
        return;
    }

    FVector TeleportLocation = AIPawn->GetActorLocation();
    FQuat TeleportRotation = AIPawn->GetActorQuat();

    if (bPlayerInNormal)
    {
        // ---- IDA: bPlayerInNormal true path → AI goes UP (to match normal world) ----
        FVector RayStart = TeleportLocation;
        RayStart.Z += TeleportDistance;
        FVector RayEnd = RayStart;
        RayEnd.Y = TeleportLocation.Y;
        RayEnd.Z = RayStart.Z - 50.0f;

        FVector NavHitLocation;
        bNavMeshBlocked = UNavigationSystemV1::NavigationRaycast(
            World, RayStart, RayEnd, NavHitLocation, FilterClass, AIController);

        if (!bNavMeshBlocked)
        {
            NavHitLocation.Z += CapsuleHeight;
            AIPawn->SetActorLocationAndRotation(NavHitLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
            CurrentAIPawn = nullptr;
            AIEQSGuard.Unlock();
            bAIInNormal = true;
            return;
        }

        UEnvQuery* Query = LoadEQSQuery(TeleportationQueryARAI,
            TEXT("/Game/Blueprints/AI/EnvQueries/EQAIWorldSwitchAR.EQAIWorldSwitchAR"));

        if (!Query || !Query->IsValidLowLevel())
        {
            UE_LOG(LogTeleport, Error,
                TEXT("TeleportAI(): EQS File (/Game/Blueprints/AI/EnvQueries/EQAIWorldSwitchAR.EQAIWorldSwitchAR) was not loaded!"));
            TSharedPtr<FEnvQueryResult> EmptyResult;
            TeleportQueryFinishARAI(EmptyResult);
            bAIInNormal = true;
            return;
        }

        UE_LOG(LogTeleport, Log, TEXT("TeleportAI(): AR Branch Direct Nav Failed, Beginning EQS"));

        FEnvQueryRequest QueryRequest(Query);
        QueryRequest.Execute(EEnvQueryRunMode::SingleResult,
            FQueryFinishedSignature::CreateUObject(this, &UTeleportationSubsystem::TeleportQueryFinishARAI));

        bAIInNormal = true;
    }
    else
    {
        // ---- IDA: bPlayerInNormal false path → AI goes DOWN (to match AR world) ----
        FVector RayStart = TeleportLocation;
        RayStart.Z -= TeleportDistance;
        FVector RayEnd = RayStart;
        RayEnd.Y = TeleportLocation.Y;
        RayEnd.Z = RayStart.Z - 50.0f;

        FVector NavHitLocation;
        bNavMeshBlocked = UNavigationSystemV1::NavigationRaycast(
            World, RayStart, RayEnd, NavHitLocation, FilterClass, AIController);

        if (!bNavMeshBlocked)
        {
            NavHitLocation.Z += CapsuleHeight;
            AIPawn->SetActorLocationAndRotation(NavHitLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
            CurrentAIPawn = nullptr;
            AIEQSGuard.Unlock();
            bAIInNormal = false;
            return;
        }

        UEnvQuery* Query = LoadEQSQuery(TeleportationQueryNormalAI,
            TEXT("/Game/Blueprints/AI/EnvQueries/EQAIWorldSwitchNormal.EQAIWorldSwitchNormal"));

        if (!Query || !Query->IsValidLowLevel())
        {
            UE_LOG(LogTeleport, Error,
                TEXT("TeleportAI(): EQS File (/Game/Blueprints/AI/EnvQueries/EQAIWorldSwitchNormal.EQAIWorldSwitchNormal) was not loaded!"));
            TSharedPtr<FEnvQueryResult> EmptyResult;
            TeleportQueryFinishNormalAI(EmptyResult, AIPawn, CapsuleHeight);
            CurrentAIPawn = nullptr;
            AIEQSGuard.Unlock();
            bAIInNormal = false;
            return;
        }

        UE_LOG(LogTeleport, Log, TEXT("TeleportAI(): Normal Branch Direct Nav Failed, Beginning EQS"));

        FEnvQueryRequest QueryRequest(Query);
        QueryRequest.Execute(EEnvQueryRunMode::SingleResult,
            FQueryFinishedSignature::CreateUObject(this,
                static_cast<void(UTeleportationSubsystem::*)(TSharedPtr<FEnvQueryResult>)>(
                    &UTeleportationSubsystem::TeleportQueryFinishNormalAI)));

        bAIInNormal = false;
    }
}

//
// which triggers the "Hope And Pray" fallback (ProjectPointToNavigation).
void UTeleportationSubsystem::OnQueryTimeElapsedNormal()
{
    TSharedPtr<FEnvQueryResult> EmptyResult;
    TeleportQueryFinishNormal(EmptyResult);
}

void UTeleportationSubsystem::OnQueryTimeElapsedAR()
{
    TSharedPtr<FEnvQueryResult> EmptyResult;
    TeleportQueryFinishAR(EmptyResult);
}

//
// EQS callback for Normal→AR player teleport.
//
//   1. EQS succeeded (result && status == 1): use GetItemAsLocation
//   2. EQS failed ("Hope And Pray"): ProjectPointToNavigation from
//      player pos + (-5000 Z), then add CassieCapsule offset
//
// Both paths: SetActorLocationAndRotation, then TeleportFinishHelper(false)
void UTeleportationSubsystem::TeleportQueryFinishNormal(TSharedPtr<FEnvQueryResult> Result)
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishNormal(): Retrieved World is invalid!"));
        return;
    }

    if (EQSFallbackTimerHandle.IsValid())
    {
        World->GetTimerManager().ClearTimer(EQSFallbackTimerHandle);
        EQSFallbackTimerHandle.Invalidate();
    }

    ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
    if (!PlayerChar)
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishNormal(): Retrieved Player Character is invalid!"));
        return;
    }

    FQuat TeleportRotation = PlayerChar->GetActorQuat();
    FVector FinalLocation;

    if (Result.IsValid() && Result->GetRawStatus() == EEnvQueryStatus::Success)
    {
        UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishNormal(): EQ Finished"));
        FinalLocation = Result->GetItemAsLocation(0);
    }
    else
    {
        UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishNormal(): Hope And Pray Finished"));

        FVector PlayerLocation = PlayerChar->GetActorLocation();
        FVector ProjectPoint = PlayerLocation;
        ProjectPoint.Z -= TeleportDistance;

        FVector ProjectedLocation;
        UNavigationSystemV1::K2_ProjectPointToNavigation(World, ProjectPoint, ProjectedLocation, NavData, FilterClass, QueryExtent);
        FinalLocation = ProjectedLocation;
        FinalLocation.Z += CassieCapsule;
    }

    PlayerChar->SetActorLocationAndRotation(FinalLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
    TeleportFinishHelper(false, World, PlayerChar);
}

//
// EQS callback for AR→Normal player teleport. Mirror of Normal version
// but projects +5000 Z and finishes with TeleportFinishHelper(true).
void UTeleportationSubsystem::TeleportQueryFinishAR(TSharedPtr<FEnvQueryResult> Result)
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishAR(): Retrieved World is invalid!"));
        return;
    }

    if (EQSFallbackTimerHandle.IsValid())
    {
        World->GetTimerManager().ClearTimer(EQSFallbackTimerHandle);
        EQSFallbackTimerHandle.Invalidate();
    }

    ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
    if (!PlayerChar)
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishAR(): Retrieved Player Character is invalid!"));
        return;
    }

    FQuat TeleportRotation = PlayerChar->GetActorQuat();
    FVector FinalLocation;

    if (Result.IsValid() && Result->GetRawStatus() == EEnvQueryStatus::Success)
    {
        UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishAR(): EQ Finished"));
        FinalLocation = Result->GetItemAsLocation(0);
    }
    else
    {
        UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishAR(): Hope And Pray Finished"));

        FVector PlayerLocation = PlayerChar->GetActorLocation();
        FVector ProjectPoint = PlayerLocation;
        ProjectPoint.Z += TeleportDistance;

        FVector ProjectedLocation;
        UNavigationSystemV1::K2_ProjectPointToNavigation(World, ProjectPoint, ProjectedLocation, NavData, FilterClass, QueryExtent);
        FinalLocation = ProjectedLocation;
        FinalLocation.Z += CassieCapsule;
    }

    PlayerChar->SetActorLocationAndRotation(FinalLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
    TeleportFinishHelper(true, World, PlayerChar);
}

//
// EQS callback for AI teleporting to Normal world.
// Uses CurrentAIPawn. On "Hope And Pray": projects -5000 Z.
void UTeleportationSubsystem::TeleportQueryFinishNormalAI(TSharedPtr<FEnvQueryResult> Result)
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishNormalAI(): Retrieved World is invalid!"));
        goto Cleanup;
    }

    if (!CurrentAIPawn)
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishNormalAI(): Retrieved AI is invalid!"));
        goto Cleanup;
    }

    {
        FQuat TeleportRotation = CurrentAIPawn->GetActorQuat();
        FVector FinalLocation;

        if (Result.IsValid() && Result->GetRawStatus() == EEnvQueryStatus::Success)
        {
            UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishNormalAI(): EQ Finished"));
            FinalLocation = Result->GetItemAsLocation(0);
        }
        else
        {
            UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishNormalAI(): Hope And Pray Finished"));

            FVector AILocation = CurrentAIPawn->GetActorLocation();
            FVector ProjectPoint = AILocation;
            ProjectPoint.Z -= TeleportDistance;

            FVector ProjectedLocation;
            UNavigationSystemV1::K2_ProjectPointToNavigation(World, ProjectPoint, ProjectedLocation, NavData, FilterClass, QueryExtent);
            FinalLocation = ProjectedLocation;
            FinalLocation.Z += CurrentAICapsuleHeight;
        }

        CurrentAIPawn->SetActorLocationAndRotation(FinalLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
    }

Cleanup:
    CurrentAIPawn = nullptr;
    AIEQSGuard.Unlock();
}

//
// Called when EQS query asset fails to load during TeleportAI.
// Takes explicit AIPawn and CapsuleHeight parameters.
void UTeleportationSubsystem::TeleportQueryFinishNormalAI(TSharedPtr<FEnvQueryResult> Result, APawn* AIPawn, float CapsuleHeight)
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishNormalAI(): Retrieved World is invalid!"));
        goto Cleanup;
    }

    if (!AIPawn)
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishNormalAI(): Retrieved AI is invalid!"));
        goto Cleanup;
    }

    {
        FQuat TeleportRotation = AIPawn->GetActorQuat();
        FVector FinalLocation;

        if (Result.IsValid() && Result->GetRawStatus() == EEnvQueryStatus::Success)
        {
            UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishNormalAI(): EQ Finished"));
            FinalLocation = Result->GetItemAsLocation(0);
        }
        else
        {
            UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishNormalAI(): Hope And Pray Finished"));

            FVector AILocation = AIPawn->GetActorLocation();
            FVector ProjectPoint = AILocation;
            ProjectPoint.Z -= TeleportDistance;

            FVector ProjectedLocation;
            UNavigationSystemV1::K2_ProjectPointToNavigation(World, ProjectPoint, ProjectedLocation, NavData, FilterClass, QueryExtent);
            FinalLocation = ProjectedLocation;
            FinalLocation.Z += CassieCapsule;
        }

        AIPawn->SetActorLocationAndRotation(FinalLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
    }

Cleanup:
    CurrentAIPawn = nullptr;
    AIEQSGuard.Unlock();
}

//
// EQS callback for AI teleporting to AR world.
// Uses CurrentAIPawn. On "Hope And Pray": projects +5000 Z.
void UTeleportationSubsystem::TeleportQueryFinishARAI(TSharedPtr<FEnvQueryResult> Result)
{
    UWorld* World = GetWorld();
    if (!World || !World->IsValidLowLevel())
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishARAI(): Retrieved World is invalid!"));
        goto Cleanup;
    }

    if (!CurrentAIPawn)
    {
        UE_LOG(LogTeleport, Error, TEXT("TeleportQueryFinishARAI(): Retrieved AI is invalid!"));
        goto Cleanup;
    }

    {
        FQuat TeleportRotation = CurrentAIPawn->GetActorQuat();
        FVector FinalLocation;

        if (Result.IsValid() && Result->GetRawStatus() == EEnvQueryStatus::Success)
        {
            UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishARAI(): EQ Finished"));
            FinalLocation = Result->GetItemAsLocation(0);
        }
        else
        {
            UE_LOG(LogTeleport, Log, TEXT("TeleportQueryFinishARAI(): Hope And Pray Finished"));

            FVector AILocation = CurrentAIPawn->GetActorLocation();
            FVector ProjectPoint = AILocation;
            ProjectPoint.Z += TeleportDistance;

            FVector ProjectedLocation;
            UNavigationSystemV1::K2_ProjectPointToNavigation(World, ProjectPoint, ProjectedLocation, NavData, FilterClass, QueryExtent);
            FinalLocation = ProjectedLocation;
            FinalLocation.Z += CurrentAICapsuleHeight;
        }

        CurrentAIPawn->SetActorLocationAndRotation(FinalLocation, TeleportRotation, false, nullptr, ETeleportType::TeleportPhysics);
    }

Cleanup:
    CurrentAIPawn = nullptr;
    AIEQSGuard.Unlock();
}

//
// OnGameDataLoaded: reads ChowdaSaveData->bPlayerInNormal → bPlayerInNormal
// OnStoreGameData: writes bPlayerSaveInNormal → ChowdaSaveData->bPlayerInNormal
//                  only if NOT in chapter replay (!bInChapterReplay)
void UTeleportationSubsystem::OnGameDataLoaded_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        bPlayerInNormal = ChowdaSave->bPlayerInNormal;
    }
}

void UTeleportationSubsystem::OnStoreGameData_Implementation(UFNAFSaveData* SaveDataObject)
{
    if (!SaveDataObject)
    {
        return;
    }

    UFNAFChowdaSaveData* ChowdaSave = Cast<UFNAFChowdaSaveData>(SaveDataObject);
    if (ChowdaSave)
    {
        if (!ChowdaSave->bInChapterReplay)
        {
            ChowdaSave->bPlayerInNormal = bPlayerSaveInNormal;
        }
    }
}