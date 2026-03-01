#include "AISpawnSystem.h"
#include "AIManagementSystem.h"
#include "SeekerPatrolPath.h"

AAISpawnSystem::AAISpawnSystem()
{
    PrimaryActorTick.bCanEverTick = true;
}

void AAISpawnSystem::BeginPlay()
{
    Super::BeginPlay();

    UWorld* World = GetWorld();
    if (World)
    {
        AIManagementSystem = World->GetSubsystem<UAIManagementSystem>();
    }

    AddAIToPath();
}

// Randomly selects up to 3 patrol paths from AIManagementSystem's registered paths
void AAISpawnSystem::AddAIToPath()
{
    if (!AIManagementSystem)
    {
        return;
    }

    TArray<FWeakObjectPtr> SeekerPaths = AIManagementSystem->GetRegisteredPatrolPaths();

    for (int32 i = 0; i < 3; ++i)
    {
        int32 NumPaths = SeekerPaths.Num();
        if (NumPaths <= 1)
        {
            break;
        }

        int32 RandIndex = FMath::Clamp(
            FMath::FloorToInt(FMath::FRand() * (float)NumPaths),
            0, NumPaths - 1);

        UObject* PathObj = SeekerPaths[RandIndex].Get();
        if (PathObj && PathObj->IsValidLowLevel())
        {
            SeekerPaths.RemoveAt(RandIndex);
        }
    }
}

void AAISpawnSystem::OnRollChange()
{
    for (int32 i = ExistingAI.Num() - 1; i >= 0; --i)
    {
        if (ExistingAI[i])
        {
            ExistingAI[i]->Destroy();
        }
        ExistingAI.RemoveAt(i);
    }

    AddAIToPath();
}