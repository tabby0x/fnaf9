#include "AISpawnTrigger.h"
#include "AIManagementSystem.h"
#include "Components/ArrowComponent.h"
#include "EFNAFAISubType.h"

AAISpawnTrigger::AAISpawnTrigger()
{
    // IDA: disables tick (byte 10 &= ~2u)
    PrimaryActorTick.bCanEverTick = false;

    SpawnAnyCharacter = false;
    ForceShattered = false;
    SpawnLocation = CreateDefaultSubobject<UArrowComponent>(TEXT("SpawnLocation"));
    if (SpawnLocation && RootComponent)
    {
        SpawnLocation->SetupAttachment(RootComponent);
    }
}

// IDA: OnAISpawned and OnAISpawnedFailure are BlueprintImplementableEvent
// forwarders — the _Implementation is empty, Blueprint handles it
void AAISpawnTrigger::OnAISpawned_Implementation(APawn* SpawnedPawn)
{
}

void AAISpawnTrigger::OnAISpawnedFailure_Implementation(APawn* SpawnedPawn)
{
}

// IDA: OnTriggered_Implementation — the core spawn logic
// Gets AIManagementSystem, builds type array, picks random type, spawns at SpawnLocation
void AAISpawnTrigger::OnTriggered_Implementation()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UAIManagementSystem* AIManager = World->GetSubsystem<UAIManagementSystem>();
    if (!AIManager)
    {
        return;
    }

    // Build the array of types to spawn from
    TArray<EFNAFAISpawnType> TypesToSpawn;

    if (SpawnAnyCharacter)
    {
        // IDA: hardcoded array: Chica(0), Monty(2), Roxy(1), Vanessa(3)
        TypesToSpawn.Add(EFNAFAISpawnType::Chica);    // 0
        TypesToSpawn.Add(EFNAFAISpawnType::Monty);     // 2
        TypesToSpawn.Add(EFNAFAISpawnType::Roxy);      // 1
        TypesToSpawn.Add(EFNAFAISpawnType::Vanessa);   // 3
    }
    else
    {
        // Use the configured SpawnCharacters array
        TypesToSpawn = SpawnCharacters;
    }

    if (TypesToSpawn.Num() <= 0)
    {
        return;
    }

    // IDA: random pick from array, clamped to valid range
    int32 RandIndex = FMath::Clamp(
        FMath::FloorToInt(FMath::FRand() * (float)TypesToSpawn.Num()),
        0, TypesToSpawn.Num() - 1);

    EFNAFAISpawnType ChosenType = TypesToSpawn[RandIndex];

    // IDA: ForceShattered → EFNAFAISubType::Shattered, else 4 (None)
    EFNAFAISubType SubType = ForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;

    // IDA: SpawnAITypeWithTransformSafeWithSubType using SpawnLocation's world transform
    // with AdjustIfPossibleButAlwaysSpawn collision handling
    bool bSuccess = false;
    APawn* SpawnedPawn = AIManager->SpawnAITypeWithTransformSafeWithSubType(
        ChosenType,
        SpawnLocation->GetComponentTransform(),
        SubType,
        bSuccess,
        ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn,
        nullptr);

    if (SpawnedPawn)
    {
        if (bSuccess)
        {
            // IDA: deactivate trigger and save state
            SetTriggerActive(false);
            SaveActivated();

            // IDA: call BlueprintImplementableEvent
            OnAISpawned(SpawnedPawn);

            // IDA: broadcast delegate if bound
            if (OnAISpawnedDelegate.IsBound())
            {
                OnAISpawnedDelegate.Broadcast(SpawnedPawn);
            }
        }
        else
        {
            // IDA: spawn returned a pawn but success was false
            OnAISpawnedFailure(SpawnedPawn);

            if (OnAISpawnedFailureDelegate.IsBound())
            {
                OnAISpawnedFailureDelegate.Broadcast(SpawnedPawn);
            }
        }
    }
}