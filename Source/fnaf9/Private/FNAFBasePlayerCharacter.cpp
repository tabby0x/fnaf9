#include "FNAFBasePlayerCharacter.h"
#include "StreamingLevelUtil.h"
#include "WorldStateSystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/GameInstance.h"
#include "Blueprint/UserWidget.h"
#include "FNAFBasePlayerController.h"

AFNAFBasePlayerCharacter::AFNAFBasePlayerCharacter()
{
    PrimaryActorTick.bCanEverTick = true;
    PawnType = EPlayerPawnType::Gregory; // test to see if in fnaf sb this was the default value? Because the class is fully reimplemented, yet nothing seems to let me interact.
}

// Freddy pawns register themselves with WorldStateSystem so other systems can find them at runtime
void AFNAFBasePlayerCharacter::BeginPlay()
{
    if (PawnType == EPlayerPawnType::Freddy)
    {
        UGameInstance* GI = GetGameInstance();
        if (GI)
        {
            UWorldStateSystem* WSS = GI->GetSubsystem<UWorldStateSystem>();
            if (WSS)
            {
                WSS->FreddyPawn = this;
            }
        }
    }

    Super::BeginPlay();
}

void AFNAFBasePlayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    if (PawnType == EPlayerPawnType::Freddy)
    {
        UGameInstance* GI = GetGameInstance();
        if (GI)
        {
            UWorldStateSystem* WSS = GI->GetSubsystem<UWorldStateSystem>();
            if (WSS)
            {
                WSS->FreddyPawn = nullptr;
            }
        }
    }
}

EPlayerPawnType AFNAFBasePlayerCharacter::GetPlayerPawnType_Implementation() const
{
    return PawnType;
}

/* Retrieves the last saved location from WorldStateSystem.
   Uses GetSavedPlayerLocationAndRotation for Gregory,
   GetSavedFreddyLocationAndRotation for Freddy. */
void AFNAFBasePlayerCharacter::GetLastSavedLocationAndRotation(
    FVector& LastSavedLocation,
    FRotator& LastSavedRotation)
{
    UGameInstance* GI = GetGameInstance();
    if (!GI)
    {
        return;
    }

    UWorldStateSystem* WSS = GI->GetSubsystem<UWorldStateSystem>();
    if (!WSS)
    {
        return;
    }

    if (PawnType == EPlayerPawnType::Gregory)
    {
        WSS->GetSavedPlayerLocationAndRotation(LastSavedLocation, LastSavedRotation);
    }
    else if (PawnType == EPlayerPawnType::Freddy)
    {
        bool bFreddyInWorld = false;
        WSS->GetSavedFreddyLocationAndRotation(bFreddyInWorld, LastSavedLocation, LastSavedRotation);
    }
}

// Also snaps the spring arm component to avoid camera lag during teleport
void AFNAFBasePlayerCharacter::TeleportPlayer(const FVector& WorldLocation, float Yaw)
{
    FVector EulerAngles(0.0f, 0.0f, Yaw);
    FRotator NewRotation = FRotator::MakeFromEuler(EulerAngles);

    TeleportTo(WorldLocation, NewRotation, false, false);

    if (Controller)
    {
        Controller->SetControlRotation(NewRotation);
    }

    // Snap the spring arm to the new position to prevent camera lerp
    USpringArmComponent* SpringArm = FindComponentByClass<USpringArmComponent>();
    if (SpringArm)
    {
        /* The binary directly writes to the spring arm's internal previous state
           to prevent camera interpolation. The cleanest UE4 equivalent: */
        FVector ArmOffset = SpringArm->GetRelativeLocation();
        FVector DesiredLocation = WorldLocation + ArmOffset;
        SpringArm->bDoCollisionTest = SpringArm->bDoCollisionTest; // force update
    }
}

/* Teleports such that the camera ends up at CameraWorldLocation.
   Computes the player location by subtracting the spring arm's relative offset. */
void AFNAFBasePlayerCharacter::TeleportPlayerWithCameraLocation(
    const FVector& CameraWorldLocation,
    float Yaw)
{
    USpringArmComponent* SpringArm = FindComponentByClass<USpringArmComponent>();
    if (!SpringArm)
    {
        TeleportPlayer(CameraWorldLocation, Yaw);
        return;
    }

    FVector ArmOffset = SpringArm->GetRelativeLocation();
    FVector PlayerLocation = CameraWorldLocation - ArmOffset;

    TeleportPlayer(PlayerLocation, Yaw);
}

/* Called when the player falls into a kill zone. Only handles Gregory and
   Freddy pawns. Disables gravity and input, then starts async-loading all
   streaming levels at the last saved location. When loading completes,
   OnKillZLevelsLoaded is called to finalize recovery. */
void AFNAFBasePlayerCharacter::FellOutOfWorld(const UDamageType& dmgType)
{
    if (PawnType != EPlayerPawnType::Gregory && PawnType != EPlayerPawnType::Freddy)
    {
        return;
    }

    if (GetCharacterMovement())
    {
        GetCharacterMovement()->GravityScale = 0.0f;
    }

    APlayerController* PC = Cast<APlayerController>(Controller);
    if (PC)
    {
        DisableInput(PC);
        PC->DisableInput(PC);
    }

    FVector ResetLocation;
    FRotator ResetRotation;
    GetLastSavedLocationAndRotation(ResetLocation, ResetRotation);

    FLatentActionInfo LatentInfo;
    LatentInfo.CallbackTarget = this;
    LatentInfo.ExecutionFunction = FName("OnKillZLevelsLoaded");
    LatentInfo.Linkage = 0;
    LatentInfo.UUID = 0;

    UStreamingLevelUtil::LoadStreamingLevelsAtLocation(this, ResetLocation, false, LatentInfo);
}

// Latent callback after levels finish loading: teleports player, restores gravity and input
void AFNAFBasePlayerCharacter::OnKillZLevelsLoaded()
{
    FVector ResetLocation;
    FRotator ResetRotation;
    GetLastSavedLocationAndRotation(ResetLocation, ResetRotation);
    TeleportPlayer(ResetLocation, ResetRotation.Yaw);

    UStreamingLevelUtil::EnableAllStreamingVolumes(this, true);

    if (GetCharacterMovement())
    {
        GetCharacterMovement()->GravityScale = 1.0f;
    }

    APlayerController* PC = Cast<APlayerController>(Controller);
    if (PC)
    {
        EnableInput(PC);
        PC->EnableInput(PC);
    }
}
