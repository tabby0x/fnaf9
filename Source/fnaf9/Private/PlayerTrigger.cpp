#include "PlayerTrigger.h"
#include "Components/BoxComponent.h"
#include "Components/ShapeComponent.h"
#include "WorldStateSystem.h"
#include "FNAFGameInstanceBase.h"
#include "Kismet/GameplayStatics.h"

APlayerTrigger::APlayerTrigger()
{
    TriggerComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerCollider"));
    RootComponent = TriggerComponent;

    bUseContinuousTrigger = false;
    SaveName = NAME_None;
    bSaveOnTrigger = false;

    ValidGameTypes.Add(EFNAFGameType::Normal);

    bFixCollision = true;
    bTriggerOnActorOverlap = true;

    bIsCurrentlyFixingCollision = false;

    PrimaryActorTick.bCanEverTick = true;
}

/* If bFixCollision: temporarily disables collision on all shape children,
   fixes collision settings, then re-enables. This prevents false overlaps
   during actor initialization. Then sets initial active state from world
   state and binds to WorldStateSystem::OnObjectStateChanged. */
void APlayerTrigger::BeginPlay()
{
    if (bFixCollision)
    {
        bIsCurrentlyFixingCollision = true;

        TInlineComponentArray<UShapeComponent*> ShapeComponents;
        GetComponents<UShapeComponent>(ShapeComponents);

        for (UShapeComponent* Shape : ShapeComponents)
        {
            Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Shape->SetCollisionResponseToChannel(ECollisionChannel::ECC_GameTraceChannel7, ECollisionResponse::ECR_Overlap);
            Shape->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        }

        bIsCurrentlyFixingCollision = false;
    }

    Super::BeginPlay();

    SetActiveStateFromWorldState();

    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UWorldStateSystem* WorldState = GameInstance->GetSubsystem<UWorldStateSystem>();
        if (WorldState)
        {
            WorldState->OnObjectStateChanged.AddDynamic(this, &APlayerTrigger::OnWorldObjectStateChanged);
        }
    }
}

/* Checks two criteria to decide if trigger should be active:
   1. Game type: compares CurrentGameType against ValidGameTypes array
   2. Save state: if SaveName is set, checks WorldStateSystem::IsActivated
   If either check fails, deactivates the trigger. */
void APlayerTrigger::SetActiveStateFromWorldState()
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFGameInstanceBase* FNAFGameInstance = Cast<UFNAFGameInstanceBase>(GameInstance);
        if (FNAFGameInstance)
        {
            EFNAFGameType CurrentType = FNAFGameInstance->GetCurrentGameType();
            bool bGameTypeValid = false;
            for (const EFNAFGameType& ValidType : ValidGameTypes)
            {
                if (ValidType == CurrentType)
                {
                    bGameTypeValid = true;
                    break;
                }
            }

            if (!bGameTypeValid)
            {
                SetTriggerActive(false);
                return;
            }
        }
    }

    if (SaveName != NAME_None)
    {
        UGameInstance* GI = GetGameInstance();
        if (GI)
        {
            UWorldStateSystem* WorldState = GI->GetSubsystem<UWorldStateSystem>();
            if (WorldState)
            {
                if (WorldState->IsActivated(SaveName))
                {
                    SetTriggerActive(false);
                    return;
                }
            }
        }
    }

    SetTriggerActive(true);
}

// Enables/disables collision on all shape children. Tick only enabled if bUseContinuousTrigger AND active.
void APlayerTrigger::SetTriggerActive(bool bActive)
{
    TInlineComponentArray<UShapeComponent*> ShapeComponents;
    GetComponents<UShapeComponent>(ShapeComponents);

    for (UShapeComponent* Shape : ShapeComponents)
    {
        Shape->SetCollisionEnabled(bActive ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    }

    SetActorTickEnabled(bUseContinuousTrigger && bActive);
}

// Only fires for the player pawn, guarded by bIsCurrentlyFixingCollision to avoid false overlaps during BeginPlay
void APlayerTrigger::NotifyActorBeginOverlap(AActor* OtherActor)
{
    if (bTriggerOnActorOverlap && !bIsCurrentlyFixingCollision)
    {
        APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
        if (!PlayerPawn || OtherActor != PlayerPawn)
        {
            return;
        }

        if (TriggerComponent->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
        {
            if (CanTrigger())
            {
                ForceTrigger();
            }
        }
    }
}

// Active when bUseContinuousTrigger is true. Calls OnTriggerStay if player pawn is among overlapping actors.
void APlayerTrigger::Tick(float DeltaTime)
{

    if (bTriggerOnActorOverlap
        && (TriggerComponent->GetCollisionEnabled() != ECollisionEnabled::NoCollision || !CanTrigger()))
    {
        APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);

        TArray<AActor*> OverlappingActors;
        GetOverlappingActors(OverlappingActors);

        for (AActor* Actor : OverlappingActors)
        {
            if (Actor == PlayerPawn)
            {
                OnTriggerStay();
                break;
            }
        }
    }
}

void APlayerTrigger::ForceTrigger()
{
    OnTriggered();

    if (OnPlayerTriggered.IsBound())
    {
        OnPlayerTriggered.Broadcast();
    }

    SaveActivated();
}

// If SaveName is set, registers this trigger as activated in WorldStateSystem and deactivates it
void APlayerTrigger::SaveActivated()
{
    if (SaveName != NAME_None)
    {
        UGameInstance* GameInstance = GetGameInstance();
        if (GameInstance)
        {
            UWorldStateSystem* WorldState = GameInstance->GetSubsystem<UWorldStateSystem>();
            if (WorldState)
            {
                WorldState->AddActivated(SaveName);
                SetTriggerActive(false);
            }
        }
    }
}

bool APlayerTrigger::IsTriggerActive() const
{
    return TriggerComponent->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
}

bool APlayerTrigger::IsTriggerStateSet() const
{
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UWorldStateSystem* WorldState = GameInstance->GetSubsystem<UWorldStateSystem>();
        if (WorldState)
        {
            return WorldState->IsActivated(SaveName);
        }
    }
    return false;
}

void APlayerTrigger::OnWorldObjectStateChanged(FName ObjectName, bool ObjectState)
{
    SetActiveStateFromWorldState();
}

// ISaveHandlerInterface: re-evaluates trigger state on checkpoint load (may need to deactivate if already triggered)
void APlayerTrigger::OnCheckpointLoad_Implementation(UFNAFSaveData* SaveDataObject)
{
    SetActiveStateFromWorldState();
}

void APlayerTrigger::OnTriggered_Implementation()
{
}

void APlayerTrigger::OnTriggerStay_Implementation()
{
}

bool APlayerTrigger::CanTrigger_Implementation() const
{
    return true;
}
