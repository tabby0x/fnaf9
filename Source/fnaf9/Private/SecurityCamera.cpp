/*
 * Security cameras used by the Fazwatch camera system and Ruin camera stations.
 * Each camera has two SightComponents:
 *   - PlayerDetector: detects the player (triggers camera alerts)
 *   - EnemyDetector: detects enemies implementing ICameraTrigger (Frustum mode)
 * Cameras self-register with USecurityCameraSystem on BeginPlay and unregister on EndPlay.
 */

#include "SecurityCamera.h"
#include "SecurityCameraSystem.h"
#include "SightComponent.h"
#include "FNAFMissionSystem.h"
#include "FNAFInventorySystem.h"
#include "CameraTrigger.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

ASecurityCamera::ASecurityCamera()
{
    FazwatchAccessible = true;
    bPlayerDetectorStartsOn = true;
    bEnemyDetectorStartsOn = true;
    bPlayerDetected = false;
    CameraInventoryName = NAME_None;
    OriginalRenderTarget = nullptr;
    OfficeModeRenderTarget = nullptr;

    PrimaryActorTick.bCanEverTick = true;

    PanMin = -45.0f;
    PanMax = 45.0f;
    TiltMin = -45.0f;
    TiltMax = 0.0f;

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));

    CameraPivot = CreateDefaultSubobject<USceneComponent>(TEXT("CameraPivot"));
    CameraPivot->SetupAttachment(RootComponent);

    PlayerDetector = CreateDefaultSubobject<USightComponent>(TEXT("CameraVision"));
    PlayerDetector->SetupAttachment(CameraPivot);

    PlayerControlledPivot = CreateDefaultSubobject<USceneComponent>(TEXT("PlayerControlledPivot"));
    PlayerControlledPivot->SetupAttachment(RootComponent);

    EnemyDetector = CreateDefaultSubobject<USightComponent>(TEXT("EnemyDetector"));
    EnemyDetector->SetupAttachment(CameraPivot);

    EnemyDetector->SetSightType(ESightType::Frustum);
    EnemyDetector->SetIncludeTags({ FName(TEXT("Enemy")) });
}

void ASecurityCamera::BeginPlay()
{
    UWorld* World = GetWorld();
    if (World)
    {
        USecurityCameraSystem* CamSystem = World->GetSubsystem<USecurityCameraSystem>();
        if (CamSystem)
        {
            CamSystem->RegisteredCameras.AddUnique(this);
            CamSystem->OnCameraRegistered.Broadcast(this);
        }
    }

    EnemyDetector->SetSightEnabled(bEnemyDetectorStartsOn);
    PlayerDetector->SetSightEnabled(bPlayerDetectorStartsOn);

    PlayerDetector->OnSightChanged.AddDynamic(this, &ASecurityCamera::OnPlayerDetectorSightChanged);
    EnemyDetector->OnSightChanged.AddDynamic(this, &ASecurityCamera::OnEnemyDetectorSightChanged);

    // Super::BeginPlay called last (matches original binary)
    Super::BeginPlay();
}

void ASecurityCamera::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (PlayerDetector)
    {
        PlayerDetector->OnSightChanged.RemoveDynamic(this, &ASecurityCamera::OnPlayerDetectorSightChanged);
    }

    if (EnemyDetector)
    {
        EnemyDetector->OnSightChanged.RemoveDynamic(this, &ASecurityCamera::OnEnemyDetectorSightChanged);
    }

    UWorld* World = GetWorld();
    if (World)
    {
        USecurityCameraSystem* CamSystem = World->GetSubsystem<USecurityCameraSystem>();
        if (CamSystem)
        {
            CamSystem->RegisteredCameras.Remove(this);
            CamSystem->OnCameraUnregistered.Broadcast(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}

/* Dynamically adjusts the enemy detector's frustum to match the capture component's FOV. */
void ASecurityCamera::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    USceneCaptureComponent2D* SceneCapture = GetPlayerSceneCapture();
    if (!SceneCapture)
    {
        return;
    }

    UTextureRenderTarget2D* TextureTarget = SceneCapture->TextureTarget;
    if (!TextureTarget)
    {
        return;
    }

    float HalfFOV = SceneCapture->FOVAngle * 0.5f;

    // Compute vertical half-angle from aspect ratio
    float TanHalfFOV = FMath::Tan(FMath::DegreesToRadians(HalfFOV));
    float AspectScale = TanHalfFOV * (float)TextureTarget->SizeY / (float)TextureTarget->SizeX;
    AspectScale = FMath::Clamp(AspectScale, -1.0f, 1.0f);
    float VerticalHalfAngleDeg = FMath::RadiansToDegrees(FMath::Asin(AspectScale));

    EnemyDetector->SetFrustumTypeParams(
        PanMin - HalfFOV,
        PanMax + HalfFOV,
        TiltMin - VerticalHalfAngleDeg,
        TiltMax + VerticalHalfAngleDeg);
}

void ASecurityCamera::OnConstruction(const FTransform& Transform)
{
    EnemyDetector->SetFrustumTypeParams(PanMin, PanMax, TiltMin, TiltMax);
    EnemyDetector->SetSightType(ESightType::Frustum);
}

/*
 * The bVisible delegate parameter is unused; state is toggled instead.
 * OnSightChanged fires on detection (true) then loss (false), so each call toggles bPlayerDetected.
 */
void ASecurityCamera::OnPlayerDetectorSightChanged(AActor* UpdatedActor, bool bVisible)
{
    static const FName PlayerTag(TEXT("Player"));

    if (!UpdatedActor)
    {
        return;
    }

    if (UpdatedActor->ActorHasTag(PlayerTag))
    {
        if (bPlayerDetected)
        {
            bPlayerDetected = false;
        }
        else
        {
            bPlayerDetected = true;

            UWorld* World = GetWorld();
            if (World)
            {
                USecurityCameraSystem* CamSystem = World->GetSubsystem<USecurityCameraSystem>();
                if (CamSystem)
                {
                    CamSystem->OnCameraAlert.Broadcast(this);
                }
            }
        }
    }
}

void ASecurityCamera::OnEnemyDetectorSightChanged(AActor* UpdatedActor, bool bVisible)
{
    if (!UpdatedActor || !UpdatedActor->GetClass())
    {
        return;
    }

    if (!UpdatedActor->GetClass()->ImplementsInterface(UCameraTrigger::StaticClass()))
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    USecurityCameraSystem* CamSystem = World->GetSubsystem<USecurityCameraSystem>();
    if (!CamSystem)
    {
        return;
    }

    TScriptInterface<ICameraTrigger> CameraTrigger;
    CameraTrigger.SetObject(UpdatedActor);
    CameraTrigger.SetInterface(static_cast<ICameraTrigger*>(
        UpdatedActor->GetInterfaceAddress(UCameraTrigger::StaticClass())));

    if (bVisible)
    {
        CamSystem->OnCameraTriggerAlert.Broadcast(this, CameraTrigger);
    }
    else
    {
        CamSystem->OnCameraTriggerLostAlert.Broadcast(this, CameraTrigger);
    }
}

/* Gates camera access behind mission progress and the CamerasOffice_Intro inventory item. */
bool ASecurityCamera::IsCameraUnlocked(bool OnlyShowCurrentMission) const
{
    if (!FazwatchAccessible)
    {
        return false;
    }

    if (OnlyShowCurrentMission)
    {
        UGameInstance* GameInstance = GetGameInstance();
        if (GameInstance)
        {
            UFNAFMissionSystem* MissionSys = GameInstance->GetSubsystem<UFNAFMissionSystem>();
            if (MissionSys)
            {
                TArray<FFNAFMissionState> ActiveMissions = MissionSys->GetActiveMissions();

                bool bFoundMatchingMission = false;
                for (const FFNAFMissionState& Mission : ActiveMissions)
                {
                    if (Mission.Status == EMissionStatus::Active
                        && ActorHasTag(Mission.Name))
                    {
                        bFoundMatchingMission = true;
                        break;
                    }
                }

                if (!bFoundMatchingMission)
                {
                    return false;
                }
            }
        }
    }

    UGameInstance* GameInstance = GetGameInstance();
    if (!GameInstance)
    {
        return false;
    }

    UFNAFInventorySystem* InventorySys = GameInstance->GetSubsystem<UFNAFInventorySystem>();
    if (!InventorySys)
    {
        return false;
    }

    static const FName CamerasOfficeIntroItem(TEXT("CamerasOffice_Intro"));
    return InventorySys->HasItem(CamerasOfficeIntroItem);
}

bool ASecurityCamera::HasDetectedEnemies() const
{
    return EnemyDetector && EnemyDetector->GetVisibleActors().Num() > 0;
}

TArray<AActor*> ASecurityCamera::GetVisibleEnemies() const
{
    if (EnemyDetector)
    {
        return EnemyDetector->GetVisibleActors();
    }
    return TArray<AActor*>();
}

UTextureRenderTarget2D* ASecurityCamera::GetCaptureTarget() const
{
    USceneCaptureComponent2D* SceneCapture = GetPlayerSceneCapture();
    if (SceneCapture)
    {
        return SceneCapture->TextureTarget;
    }
    return nullptr;
}

void ASecurityCamera::SetPlayerDetectorEnabled(bool bEnable)
{
    if (PlayerDetector)
    {
        PlayerDetector->SetSightEnabled(bEnable);
    }
}

// Typo "Enemey" is from the original game's symbol table
void ASecurityCamera::SetEnemeyDetectorEnabled(bool bEnable)
{
    if (EnemyDetector)
    {
        EnemyDetector->SetSightEnabled(bEnable);
    }
}

/* Switches the capture component to a fixed 512x512 render target for office mode. */
void ASecurityCamera::StartOfficeMode()
{
    USceneCaptureComponent2D* SceneCapture = GetPlayerSceneCapture();
    if (!SceneCapture)
    {
        return;
    }

    OriginalRenderTarget = SceneCapture->TextureTarget;
    OfficeModeRenderTarget = NewObject<UTextureRenderTarget2D>(this);

    if (OriginalRenderTarget)
    {
        OfficeModeRenderTarget->RenderTargetFormat = OriginalRenderTarget->RenderTargetFormat;
        OfficeModeRenderTarget->ClearColor = OriginalRenderTarget->ClearColor;
        OfficeModeRenderTarget->bGPUSharedFlag = OriginalRenderTarget->bGPUSharedFlag;
    }

    OfficeModeRenderTarget->InitAutoFormat(512, 512);
    OfficeModeRenderTarget->UpdateResourceImmediate(true);

    SceneCapture->TextureTarget = OfficeModeRenderTarget;
    SceneCapture->SetVisibility(true);

    OnOfficeModeStarted();
}

void ASecurityCamera::StopOfficeMode()
{
    if (!OriginalRenderTarget)
    {
        return;
    }

    USceneCaptureComponent2D* SceneCapture = GetPlayerSceneCapture();
    if (SceneCapture)
    {
        SceneCapture->TextureTarget = OriginalRenderTarget;
        SceneCapture->SetVisibility(false);
    }
}