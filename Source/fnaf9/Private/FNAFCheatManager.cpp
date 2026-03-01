#include "FNAFCheatManager.h"
#include "AIManagementSystem.h"
#include "FNAFBasePlayerCharacter.h"
#include "EPlayerPawnType.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogFNAFCheat, Log, All);

UFNAFCheatManager::UFNAFCheatManager()
{
}

void UFNAFCheatManager::ToggleDebugCloaking()
{
    UWorld* World = GetWorld();
    if (!World) return;

    UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
    if (AIMgr)
    {
        AIMgr->EnableDebugCloak = !AIMgr->EnableDebugCloak;
        UE_LOG(LogFNAFCheat, Log, TEXT("Debug Cloaking: %s"),
            AIMgr->EnableDebugCloak ? TEXT("ON") : TEXT("OFF"));
    }
}

void UFNAFCheatManager::SetDebugCloaking(bool Value)
{
    UWorld* World = GetWorld();
    if (!World) return;

    UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
    if (AIMgr)
    {
        AIMgr->EnableDebugCloak = Value;
    }
}

bool UFNAFCheatManager::GetDebugCloaking()
{
    UWorld* World = GetWorld();
    if (!World) return false;

    UAIManagementSystem* AIMgr = World->GetSubsystem<UAIManagementSystem>();
    return AIMgr ? AIMgr->EnableDebugCloak : false;
}

void UFNAFCheatManager::ToggleGodMode()
{
    APlayerController* PC = GetOuterAPlayerController();
    if (!PC) return;

    APawn* Pawn = PC->GetPawn();
    if (!Pawn) return;

    // Check if pawn is Gregory_C or a child of it
    // Gregory_C is the Blueprint class; its C++ base is AFNAFBasePlayerCharacter
    // with PawnType == Gregory
    AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
    if (!PlayerChar) return;

    EPlayerPawnType PawnType = IFNAFPawnTypeProviderInterface::Execute_GetPlayerPawnType(PlayerChar);
    if (PawnType != EPlayerPawnType::Gregory) return;

    // bCanBeJumpscared is a Blueprint member on Gregory_C, access via reflection
    FBoolProperty* JumpscareProp = CastField<FBoolProperty>(
        Pawn->GetClass()->FindPropertyByName(TEXT("CanBeJumpScared")));

    if (JumpscareProp)
    {
        bool bCurrentValue = JumpscareProp->GetPropertyValue_InContainer(Pawn);
        JumpscareProp->SetPropertyValue_InContainer(Pawn, !bCurrentValue);

        UE_LOG(LogFNAFCheat, Log, TEXT("God Mode: %s (CanBeJumpScared = %s)"),
            bCurrentValue ? TEXT("ON") : TEXT("OFF"),
            !bCurrentValue ? TEXT("true") : TEXT("false"));
    }
    else
    {
        UE_LOG(LogFNAFCheat, Warning, TEXT("ToggleGodMode: CanBeJumpScared not found on %s"),
            *Pawn->GetClass()->GetName());
    }
}

void UFNAFCheatManager::UpgradeFreddy(EFreddyUpgradeType Type)
{
    // TODO: restore from IDA
}

void UFNAFCheatManager::ApplyQualitySettings(int32 VisualQualityLevel, int32 RayTraceQualityLevel)
{
    // TODO: restore from IDA
}