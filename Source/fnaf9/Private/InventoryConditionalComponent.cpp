#include "InventoryConditionalComponent.h"
#include "FNAFBasePlayerCharacter.h"
#include "FNAFInventorySystem.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

UInventoryConditionalComponent::UInventoryConditionalComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    RequiredSecurityLevel = 0;
    RequiresPartyPass = false;
    RequiresPawn = EPlayerPawnType::None;
    PawnTypeIgnoresConditions = EPlayerPawnType::None;
}

void UInventoryConditionalComponent::BeginPlay()
{
    Super::BeginPlay();

    UWorld* World = GetWorld();
    if (!World) return;

    UGameInstance* GI = World->GetGameInstance();
    if (!GI) return;

    UFNAFInventorySystem* InventorySystem = GI->GetSubsystem<UFNAFInventorySystem>();
    if (!InventorySystem) return;

    // Bind to OnInventoryItemAdded
    InventorySystem->OnInventoryItemAdded.AddDynamic(this, &UInventoryConditionalComponent::OnItemCollected);

    // Bind to OnMessageAdded
    InventorySystem->OnMessageAdded.AddDynamic(this, &UInventoryConditionalComponent::OnMessageCollected);
}

void UInventoryConditionalComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    UWorld* World = GetWorld();
    if (!World) return;

    UGameInstance* GI = World->GetGameInstance();
    if (!GI) return;

    UFNAFInventorySystem* InventorySystem = GI->GetSubsystem<UFNAFInventorySystem>();
    if (!InventorySystem) return;

    // Unbind from OnInventoryItemAdded
    InventorySystem->OnInventoryItemAdded.RemoveDynamic(this, &UInventoryConditionalComponent::OnItemCollected);

    // Unbind from OnMessageAdded
    InventorySystem->OnMessageAdded.RemoveDynamic(this, &UInventoryConditionalComponent::OnMessageCollected);
}

bool UInventoryConditionalComponent::ConditionsMet_Implementation()
{
    UWorld* World = GetWorld();
    if (!World) return false;

    UGameInstance* GI = World->GetGameInstance();
    if (!GI) return false;

    UFNAFInventorySystem* InventorySystem = GI->GetSubsystem<UFNAFInventorySystem>();
    if (!InventorySystem) return false;

    AFNAFBasePlayerCharacter* PlayerPawn = Cast<AFNAFBasePlayerCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));

    // Check if pawn type ignores conditions entirely
    if (PawnTypeIgnoresConditions != EPlayerPawnType::None
        && PlayerPawn
        && PlayerPawn->GetPlayerPawnType() == PawnTypeIgnoresConditions)
    {
        return true;
    }

    // Check required inventory item
    if (RequiredInventoryItem != NAME_None)
    {
        if (!InventorySystem->HasItem(RequiredInventoryItem))
        {
            return false;
        }
    }

    // Check security level
    if (InventorySystem->SecurityLevel < RequiredSecurityLevel)
    {
        return false;
    }

    // Check party pass
    if (RequiresPartyPass)
    {
        if (InventorySystem->CollectedPartyPassCount - InventorySystem->UsedPartyPassCount <= 0)
        {
            return false;
        }
    }

    // Check required pawn type
    if (RequiresPawn != EPlayerPawnType::None)
    {
        if (!PlayerPawn || PlayerPawn->GetPlayerPawnType() != RequiresPawn)
        {
            return false;
        }
    }

    return true;
}

void UInventoryConditionalComponent::HasMetConditions(bool& OutConditionsMet, EConditionFailReason& OutFailReason)
{
    OutFailReason = EConditionFailReason::None;
    OutConditionsMet = true;

    UWorld* World = GetWorld();
    if (!World)
    {
        OutConditionsMet = false;
        return;
    }

    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        OutConditionsMet = false;
        return;
    }

    UFNAFInventorySystem* InventorySystem = GI->GetSubsystem<UFNAFInventorySystem>();
    if (!InventorySystem)
    {
        OutConditionsMet = false;
        return;
    }

    AFNAFBasePlayerCharacter* PlayerPawn = Cast<AFNAFBasePlayerCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));

    // Pawn type ignores conditions
    if (PawnTypeIgnoresConditions != EPlayerPawnType::None
        && PlayerPawn
        && PlayerPawn->GetPlayerPawnType() == PawnTypeIgnoresConditions)
    {
        OutConditionsMet = true;
        return;
    }

    // Required inventory item
    if (RequiredInventoryItem != NAME_None && !InventorySystem->HasItem(RequiredInventoryItem))
    {
        OutFailReason = EConditionFailReason::ItemRequired;
        OutConditionsMet = false;
        return;
    }

    // Security level
    if (InventorySystem->SecurityLevel < RequiredSecurityLevel)
    {
        OutFailReason = EConditionFailReason::SecurityLevel;
        OutConditionsMet = false;
        return;
    }

    // Party pass
    if (RequiresPartyPass && InventorySystem->CollectedPartyPassCount - InventorySystem->UsedPartyPassCount <= 0)
    {
        OutFailReason = EConditionFailReason::PartyPass;
        OutConditionsMet = false;
        return;
    }

    // Required pawn type
    if (RequiresPawn != EPlayerPawnType::None)
    {
        if (!PlayerPawn || PlayerPawn->GetPlayerPawnType() != RequiresPawn)
        {
            OutConditionsMet = false;
            return;
        }
    }
}

void UInventoryConditionalComponent::BindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated)
{
    // Add if not already present
    int32 ExistingIndex = Delegates.Delegates.IndexOfByPredicate([&](const FOnConditionResultUpdated& Existing) {
        return Existing == OnConditionResultUpdated;
        });

    if (ExistingIndex == INDEX_NONE)
    {
        Delegates.Delegates.Add(OnConditionResultUpdated);
    }
}

void UInventoryConditionalComponent::UnbindConditionUpdatedDelegate_Implementation(const FOnConditionResultUpdated& OnConditionResultUpdated)
{
    int32 Index = Delegates.Delegates.IndexOfByPredicate([&](const FOnConditionResultUpdated& Existing) {
        return Existing == OnConditionResultUpdated;
        });

    if (Index != INDEX_NONE)
    {
        Delegates.Delegates.RemoveAt(Index);
    }
}

void UInventoryConditionalComponent::OnItemCollected(FName ItemName, FFNAFInventoryTableStruct InventoryTableStruct)
{
    NotifyDelegates();
}

void UInventoryConditionalComponent::OnMessageCollected(FName ItemName, FFNAFMessageTableStruct MessageTableStruct)
{
    NotifyDelegates();
}

void UInventoryConditionalComponent::NotifyDelegates()
{
    TScriptInterface<IConditionCheckInterface> SelfInterface;
    SelfInterface.SetObject(this);
    SelfInterface.SetInterface(static_cast<IConditionCheckInterface*>(this));
    Delegates.CallDelegates(SelfInterface);
}

void UInventoryConditionalComponent::SetNewConditions(FName NewRequiredInventoryItem, int32 NewRequiredSecurityLevel, bool NewRequiresPartyPass)
{
    RequiredInventoryItem = NewRequiredInventoryItem;
    RequiredSecurityLevel = NewRequiredSecurityLevel;
    RequiresPartyPass = NewRequiresPartyPass;
}