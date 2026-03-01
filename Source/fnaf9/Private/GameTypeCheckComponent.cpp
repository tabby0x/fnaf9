#include "GameTypeCheckComponent.h"
#include "FNAFGameInstanceBase.h"
#include "Engine/World.h"

UGameTypeCheckComponent::UGameTypeCheckComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

    // Pseudocode: Src = 512 (0x0200) = two bytes {0x00, 0x02} = {Normal, DirectMinigame}
    AllowedGameTypes.Add(EFNAFGameType::Normal);
    AllowedGameTypes.Add(EFNAFGameType::DirectMinigame);
	AllowedGameTypes.Add(EFNAFGameType::ChowdaMode);
}

void UGameTypeCheckComponent::BeginPlay()
{
    Super::BeginPlay();

    bool bAllowed = false;

    UWorld* World = GetWorld();
    if (World)
    {
        UFNAFGameInstanceBase* FNAFGI = Cast<UFNAFGameInstanceBase>(World->GetGameInstance());
        if (FNAFGI)
        {
            EFNAFGameType CurrentType = FNAFGI->GetCurrentGameType();
            bAllowed = AllowedGameTypes.Contains(CurrentType);
        }
    }

    if (bAllowed)
    {
        OnGameTypeAllowed.Broadcast();
        OnAllowedGameType();
    }
    else
    {
        OnGameTypeNotAllowed.Broadcast();
        OnNotAllowedGameType();
    }
}

bool UGameTypeCheckComponent::IsAllowed() const
{
    UWorld* World = GetWorld();
    if (!World) return false;

    UGameInstance* GI = World->GetGameInstance();
    UFNAFGameInstanceBase* FNAFGI = Cast<UFNAFGameInstanceBase>(GI);
    if (!FNAFGI) return false;

    EFNAFGameType CurrentType = FNAFGI->GetCurrentGameType();
    return AllowedGameTypes.Contains(CurrentType);
}

void UGameTypeCheckComponent::OnAllowedGameType_Implementation()
{
}

void UGameTypeCheckComponent::OnNotAllowedGameType_Implementation()
{
}