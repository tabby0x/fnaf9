/* Self-registers with UAIManagementSystem::SpawnPoints on BeginPlay,
   removes itself on EndPlay. The management system uses these weak refs
   for all spawn point queries.

   NOTE: Requires adding to AIManagementSystem.h (in the public section):
     void AddSpawnPoint(AFNAFAISpawnPoint* SpawnPoint);
     void RemoveSpawnPoint(AFNAFAISpawnPoint* SpawnPoint); */

#include "FNAFAISpawnPoint.h"
#include "AIManagementSystem.h"

AFNAFAISpawnPoint::AFNAFAISpawnPoint()
{
    PrimaryActorTick.bCanEverTick = true;

    bUseType = false;
    AIType = EFNAFAISpawnType::Chica;
    bIsStagedPoint = false;
}

void AFNAFAISpawnPoint::BeginPlay()
{
    UWorld* World = GetWorld();
    if (World)
    {
        UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
        if (AIMgr)
        {
            AIMgr->AddSpawnPoint(this);
        }
    }

    Super::BeginPlay();
}

void AFNAFAISpawnPoint::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UWorld* World = GetWorld();
    if (World)
    {
        UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
        if (AIMgr)
        {
            AIMgr->RemoveSpawnPoint(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}

void AFNAFAISpawnPoint::Destroyed()
{
    Super::Destroyed();
}

EFNAFAISpawnType AFNAFAISpawnPoint::GetAIType() const
{
    return AIType;
}
