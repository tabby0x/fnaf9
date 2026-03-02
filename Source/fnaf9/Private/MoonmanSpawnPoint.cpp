#include "MoonmanSpawnPoint.h"
#include "MoonmanManagementSystem.h"
#include "FNAFSightSystem.h"
#include "Components/BillboardComponent.h"
#include "Components/SceneComponent.h"
#include "VisualSourceComponent.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "fnaf9GameModeBase.h"

AMoonmanSpawnPoint::AMoonmanSpawnPoint()
{
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* DefaultSceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
    RootComponent = DefaultSceneRoot;
    SetRootComponent(DefaultSceneRoot);

    VisualSource = CreateDefaultSubobject<UVisualSourceComponent>(TEXT("VisualSourceComponent"));

    BillboardComponent = CreateDefaultSubobject<UBillboardComponent>(TEXT("Icon"));
    if (BillboardComponent)
    {
        BillboardComponent->SetupAttachment(RootComponent);

        static ConstructorHelpers::FObjectFinderOptional<UTexture2D> TextureMission(
            TEXT("/Game/ShadingAssets/EditorIcons/MissionIcon_Editor"));
        if (TextureMission.Get())
        {
            BillboardComponent->Sprite = TextureMission.Get();
        }

        BillboardComponent->SetHiddenInGame(true);
    }

    b_CanSpawn = true;
    bIsTriggered = true;
    bIsSingleUse = true;
    bHasSpawnedOnce = false;
    bHasLookedOnce = false;
    probabilityOfSpawn = 1.0f;
    originalProbabilityOfSpawn = 1.0f;
}

/* Registers with MoonmanManagementSystem and FNAFSightSystem, caches
   originalProbabilityOfSpawn, and binds to game mode's AI display delegate.
   Calls Super::BeginPlay LAST (IDA confirms this ordering). */
void AMoonmanSpawnPoint::BeginPlay()
{
    UWorld* World = GetWorld();
    if (World)
    {
        UMoonmanManagementSystem* MoonmanSys = World->GetSubsystem<UMoonmanManagementSystem>();
        if (MoonmanSys)
        {
            MoonmanSys->RegisterSpawn(this);
        }

        UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();
        if (SightSystem)
        {
            SightSystem->RegisterMMSpawnPoint(this);
        }
    }

    originalProbabilityOfSpawn = probabilityOfSpawn;

    /* IDA: Binds OnSetAIDisplay delegate to game mode, sets billboard
       visibility from current AI display state */
    if (World)
    {
        Afnaf9GameModeBase* FNAFGameMode = Cast<Afnaf9GameModeBase>(World->GetAuthGameMode());
        if (FNAFGameMode)
        {
            FNAFGameMode->OnSetAIDisplay.AddDynamic(this, &AMoonmanSpawnPoint::OnSetAIDisplay);
            if (BillboardComponent)
            {
                BillboardComponent->SetHiddenInGame(!FNAFGameMode->IsAIDisplayOn());
            }
        }
    }

    Super::BeginPlay();
}

void AMoonmanSpawnPoint::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UWorld* World = GetWorld();
    if (World)
    {
        UMoonmanManagementSystem* MoonmanSys = World->GetSubsystem<UMoonmanManagementSystem>();
        if (MoonmanSys)
        {
            MoonmanSys->UnRegisterSpawn(this);
        }

        UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();
        if (SightSystem)
        {
            SightSystem->UnregisterMMSpawnPoint(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}

// Called by the sight system when a moonman spawn point enters the player's sight cone
void AMoonmanSpawnPoint::DetectTheSpawnPoint(AMoonmanSpawnPoint* passed_HitActor, bool passed_bVisible)
{
    OnMMDetected.Broadcast(passed_HitActor, passed_bVisible);
}

// Toggles debug billboard visibility from game mode's AI display delegate
void AMoonmanSpawnPoint::OnSetAIDisplay(bool enable)
{
    if (BillboardComponent)
    {
        BillboardComponent->SetHiddenInGame(!enable);
    }
}

void AMoonmanSpawnPoint::SetSpawnProbability(float tempProbability)
{
    probabilityOfSpawn = tempProbability;
}

void AMoonmanSpawnPoint::SetCanSpawn(bool canSpawn)
{
    b_CanSpawn = canSpawn;
}

void AMoonmanSpawnPoint::SetIsTriggered(bool isTriggered)
{
    bIsTriggered = isTriggered;
}

void AMoonmanSpawnPoint::SetIsSpawned(bool IsSpawned)
{
    bIsSpawned = IsSpawned;
}

void AMoonmanSpawnPoint::SetHasSpawnedOnce(bool HasSpawnedOnce)
{
    bHasSpawnedOnce = HasSpawnedOnce;
}

void AMoonmanSpawnPoint::SetHasLookedOnce(bool HasLookedOnce)
{
    bHasLookedOnce = HasLookedOnce;
}

void AMoonmanSpawnPoint::SetIsMMDetected(bool IsSkeletonDetected)
{
    bIsSkeletonDetected = IsSkeletonDetected;
}

void AMoonmanSpawnPoint::SetIsSingleUse(bool In)
{
    bIsSingleUse = In;
}

void AMoonmanSpawnPoint::SetShouldFollowPlayer(bool In)
{
    bShouldFollowPlayer = In;
}

void AMoonmanSpawnPoint::SetIsStationary(bool In)
{
    bIsStationary = In;
}

void AMoonmanSpawnPoint::SetIsAnimLoop(bool In)
{
    bAnimLoop = In;
}

void AMoonmanSpawnPoint::SetAimHeadAtPlayer(bool In)
{
    bAimHeadAtPlayer = In;
}

void AMoonmanSpawnPoint::SetCanPopUp(bool In)
{
    bCanPopUp = In;
}

void AMoonmanSpawnPoint::SetMMAnimCategory(const EMMAnimCategory In)
{
    MMAnimCategory = In;
}

float AMoonmanSpawnPoint::GetSpawnProbability()
{
    return probabilityOfSpawn;
}

float AMoonmanSpawnPoint::GetOriginalProbability()
{
    return originalProbabilityOfSpawn;
}

bool AMoonmanSpawnPoint::GetCanSpawn()
{
    return b_CanSpawn;
}

bool AMoonmanSpawnPoint::GetIsTriggered()
{
    return bIsTriggered;
}

bool AMoonmanSpawnPoint::GetIsSpawned()
{
    return bIsSpawned;
}

bool AMoonmanSpawnPoint::GetHasSpawnedOnce()
{
    return bHasSpawnedOnce;
}

bool AMoonmanSpawnPoint::GetHasLookedOnce()
{
    return bHasLookedOnce;
}

bool AMoonmanSpawnPoint::GetIsMMDetected()
{
    return bIsSkeletonDetected;
}

bool AMoonmanSpawnPoint::GetIsSingleUse()
{
    return bIsSingleUse;
}

bool AMoonmanSpawnPoint::GetIsFollowPlayerTrue()
{
    return bShouldFollowPlayer;
}

bool AMoonmanSpawnPoint::GetIsStationary()
{
    return bIsStationary;
}

bool AMoonmanSpawnPoint::GetIsAnimLoop()
{
    return bAnimLoop;
}

bool AMoonmanSpawnPoint::GetAimHeadAtPlayer()
{
    return bAimHeadAtPlayer;
}

bool AMoonmanSpawnPoint::GetCanPopUp()
{
    return bCanPopUp;
}

TArray<UAnimSequence*> AMoonmanSpawnPoint::GetMMAnimSeq_Array() const
{
    return MMAnimSeq_Array;
}

EMMAnimCategory AMoonmanSpawnPoint::GetMMAnimCategory() const
{
    return MMAnimCategory;
}
