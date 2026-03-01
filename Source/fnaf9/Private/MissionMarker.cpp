#include "MissionMarker.h"
#include "Components/SceneComponent.h"
#include "FNAFMissionSystem.h"

AMissionMarker::AMissionMarker()
{
    PrimaryActorTick.bCanEverTick = false;
    MissionStateIndex = -1;
    MissionName = NAME_None;

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

// Registers this marker with UFNAFMissionSystem::MissionMarkers array.
void AMissionMarker::BeginPlay()
{
    AActor::BeginPlay();

    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFMissionSystem* MissionSystem = GameInstance->GetSubsystem<UFNAFMissionSystem>();
        if (MissionSystem)
        {
            MissionSystem->RegisterMarker(this);
        }
    }
}

void AMissionMarker::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    AActor::EndPlay(EndPlayReason);

    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFMissionSystem* MissionSystem = GameInstance->GetSubsystem<UFNAFMissionSystem>();
        if (MissionSystem)
        {
            MissionSystem->UnregisterMarker(this);
        }
    }
}

int32 AMissionMarker::GetStateIndex() const
{
    return MissionStateIndex;
}

FName AMissionMarker::GetMissionName() const
{
    return MissionName;
}
