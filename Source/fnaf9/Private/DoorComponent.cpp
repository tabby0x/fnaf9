#include "DoorComponent.h"
#include "FNAFBasePlayerCharacter.h"
#include "FNAFGameInstanceBase.h"
#include "FNAFInventorySystem.h"
#include "DoorInteractor.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogDoorComponent, Log, All);

UDoorComponent::UDoorComponent()
{
    bIsLockedForAI = false;
    bIsLockedForPlayer = false;
    bIsOpen = false;
    PlayerBlocker = nullptr;
    bIsAutomaticDoorForPlayer = true;
    bIsAutomaticDoorForAI = true;
    bCanCloseDoorForAI = false;
    bDoorHasInitialized = false;

    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    bWantsInitializeComponent = true;
}

bool UDoorComponent::IsChowdaMode() const
{
    UWorld* World = GetWorld();
    if (!World) return false;

    UFNAFGameInstanceBase* GI = Cast<UFNAFGameInstanceBase>(World->GetGameInstance());
    if (!GI) return false;

    return GI->GetCurrentGameType() == EFNAFGameType::ChowdaMode;
}

const FDoorEntryRequirements& UDoorComponent::GetActiveRequirements() const
{
    if (IsChowdaMode())
    {
        return ChowdaDoorEntryRequirements;
    }
    return NormalDoorEntryRequirements;
}

void UDoorComponent::InitializeComponent()
{
    Super::InitializeComponent();

    UWorld* World = GetWorld();
    if (!World) return;

    UFNAFGameInstanceBase* GI = Cast<UFNAFGameInstanceBase>(World->GetGameInstance());
    if (!GI) return;

    const FDoorEntryRequirements& Reqs = GetActiveRequirements();

    bIsLockedForPlayer = Reqs.bInitiallyLockedPlayer;
    bIsLockedForAI = Reqs.bInitiallyLockedAI;
    bIsOpen = Reqs.bStartOpen;
    bIsAutomaticDoorForPlayer = Reqs.bIsAutomaticDoorForPlayer;
    bIsAutomaticDoorForAI = Reqs.bIsAutomaticDoorForAI;
    bCanCloseDoorForAI = Reqs.bCanCloseDoorForAI;
    bDoorHasInitialized = true;

    if (bIsOpen)
    {
        OnInitialOpen.Broadcast();
    }

    if (PlayerBlocker && IsValid(PlayerBlocker))
    {
        PlayerBlocker->SetCollisionEnabled(bIsOpen ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
    }
}

void UDoorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bIsAutomaticDoorForPlayer)
    {
        return;
    }

    for (auto It = PawnsInDoorway.CreateIterator(); It; ++It)
    {
        APawn* Pawn = It->Key;
        if (!Pawn || !Pawn->IsPlayerControlled())
        {
            continue;
        }

        bool bConditionsMet = false;
        EConditionFailReason FailReason = EConditionFailReason::None;

        // Check conditions for this pawn
        if (!IsValid(Pawn))
        {
            bConditionsMet = false;
        }
        else
        {
            UClass* PawnClass = Pawn->GetClass();
            if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
            {
                if (IDoorInteractor::Execute_DoorEntryNotAllowed(Pawn, const_cast<UDoorComponent*>(this)))
                {
                    bConditionsMet = false;
                    goto HandleResult;
                }
            }

            AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
            if (PlayerChar && Pawn->IsPlayerControlled())
            {
                HasMetConditionsDoorSide(PlayerChar, bConditionsMet, FailReason, It->Value.SideEntered);
            }
            else
            {
                bConditionsMet = !bIsLockedForAI;
            }
        }

    HandleResult:
        if (bConditionsMet)
        {
            // Disable blocker - pawn can pass
            if (PlayerBlocker && IsValid(PlayerBlocker))
            {
                PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }

            if (!bIsOpen)
            {
                bIsOpen = true;
                if (OnOpenDoor.IsBound())
                {
                    OnOpenDoor.Broadcast(true);
                }
            }
        }
        else
        {
            // Enable blocker - pawn cannot pass
            if (PlayerBlocker && IsValid(PlayerBlocker))
            {
                PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            }

            // If door is open and this is the last pawn, close
            if (bIsOpen && PawnsInDoorway.Num() == 1 && OnCloseDoor.IsBound())
            {
                bIsOpen = false;
                OnCloseDoor.Broadcast(true);
            }
        }
    }
}

void UDoorComponent::SetSecurityLevel(int32 NewSecurityLevel)
{
    ChowdaDoorEntryRequirements.RequiredSecurityLevel = NewSecurityLevel;
    NormalDoorEntryRequirements.RequiredSecurityLevel = NewSecurityLevel;
}

void UDoorComponent::SetLockedForAI(bool bLocked)
{
    bIsLockedForAI = bLocked;
}

void UDoorComponent::SetAutomaticDoorEnableForAI(bool bEnable)
{
    bIsAutomaticDoorForAI = bEnable;
}

void UDoorComponent::SetPlayerBlocker(UPrimitiveComponent* InPlayerBlocker)
{
    // Transfer collision state from old blocker to new one, then disable old
    if (PlayerBlocker && IsValid(PlayerBlocker))
    {
        if (InPlayerBlocker && IsValid(InPlayerBlocker))
        {
            ECollisionEnabled::Type CurrentCollision = PlayerBlocker->GetCollisionEnabled();
            InPlayerBlocker->SetCollisionEnabled(CurrentCollision);
        }
        PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    if (InPlayerBlocker && IsValid(InPlayerBlocker))
    {
        bool bShouldBeOpen = false;

        if (!bIsAutomaticDoorForPlayer && bIsOpen)
        {
            // Non-automatic door that's open
            bShouldBeOpen = true;
        }
        else
        {
            // Check if any player-controlled pawn in doorway meets conditions
            for (auto It = PawnsInDoorway.CreateConstIterator(); It; ++It)
            {
                APawn* Pawn = It->Key;
                if (!Pawn || !Pawn->IsPlayerControlled())
                {
                    continue;
                }

                AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
                if (!PlayerChar)
                {
                    continue;
                }

                bool bConditionsMet = false;
                EConditionFailReason FailReason = EConditionFailReason::None;

                if (IsValid(Pawn))
                {
                    UClass* PawnClass = Pawn->GetClass();
                    if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
                    {
                        if (IDoorInteractor::Execute_DoorEntryNotAllowed(Pawn, const_cast<UDoorComponent*>(this)))
                        {
                            continue;
                        }
                    }

                    if (PlayerChar && Pawn->IsPlayerControlled())
                    {
                        HasMetConditionsDoorSide(PlayerChar, bConditionsMet, FailReason, EDoorSide::None);
                    }
                    else
                    {
                        bConditionsMet = !bIsLockedForAI;
                    }
                }

                if (bConditionsMet)
                {
                    bShouldBeOpen = true;
                    break;
                }
            }
        }

        InPlayerBlocker->SetCollisionEnabled(bShouldBeOpen ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
    }

    PlayerBlocker = InPlayerBlocker;
}

void UDoorComponent::SetLockedForPlayer(bool bLocked)
{
    bool bWasOpen = bIsOpen;
    bIsLockedForPlayer = bLocked;

    if (bLocked)
    {
        if (bWasOpen)
        {
            // Check if any non-player-controlled pawn is in the doorway
            bool bHasNonPlayer = false;
            for (auto It = PawnsInDoorway.CreateConstIterator(); It; ++It)
            {
                APawn* Pawn = It->Key;
                if (Pawn && !Pawn->IsPlayerControlled())
                {
                    bHasNonPlayer = true;
                }
            }

            if (!bHasNonPlayer)
            {
                // No non-player pawns to keep door open, close it
                bIsOpen = false;
                OnCloseDoor.Broadcast(true);
            }
        }

        // Enable blocker
        if (PlayerBlocker && IsValid(PlayerBlocker))
        {
            PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        }
    }
    else
    {
        // Unlocking
        if (bWasOpen)
        {
            return; // Already open, nothing to do
        }

        // Check if any player-controlled pawn should trigger the door to open
        for (auto It = PawnsInDoorway.CreateConstIterator(); It; ++It)
        {
            APawn* Pawn = It->Key;
            if (Pawn && Pawn->IsPlayerControlled())
            {
                PawnEnteredDoor(Pawn);
                return;
            }
        }
    }
}

void UDoorComponent::SetAutomaticDoorEnableForPlayer(bool bEnable)
{
    bIsAutomaticDoorForPlayer = bEnable;

    if (!bEnable)
    {
        return;
    }

    if (bIsOpen)
    {
        // If door is open but no pawns in doorway, close it
        if (PawnsInDoorway.Num() == 0)
        {
            bIsOpen = false;
            OnCloseDoor.Broadcast(false);
        }
        return;
    }

    // Door is closed, check if any player pawn meets conditions to open
    bool bFoundQualifyingPawn = false;
    for (auto It = PawnsInDoorway.CreateConstIterator(); It; ++It)
    {
        APawn* Pawn = It->Key;
        if (!Pawn || !Pawn->IsPlayerControlled())
        {
            continue;
        }

        AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
        // If cast fails, pass nullptr (matches binary behavior)

        bool bConditionsMet = false;
        EConditionFailReason FailReason = EConditionFailReason::None;
        HasMetConditionsDoorSide(PlayerChar, bConditionsMet, FailReason, EDoorSide::None);

        if (bConditionsMet)
        {
            bFoundQualifyingPawn = true;

            if (PlayerBlocker && IsValid(PlayerBlocker))
            {
                PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }
            bIsOpen = true;
            OnOpenDoor.Broadcast(true);
            break;
        }
    }

    // If no qualifying pawn found but auto-door was just enabled, open anyway
    if (!bIsOpen)
    {
        bIsOpen = true;
        OnOpenDoor.Broadcast(false);
    }
}

void UDoorComponent::PawnEnteredDoor(APawn* Pawn)
{
    if (!Pawn)
    {
        return;
    }

    // Store pawn with door side info
    EDoorSide Side = GetDoorSideFromActor(Pawn);
    PawnsInDoorway.Add(Pawn, FPawnInDoorwayInfo(Side));
    SetComponentTickEnabled(true);

    // Check conditions
    bool bConditionsMet = false;
    EConditionFailReason FailReason = EConditionFailReason::None;

    if (Pawn && IsValid(Pawn))
    {
        UClass* PawnClass = Pawn->GetClass();
        if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
        {
            if (IDoorInteractor::Execute_DoorEntryNotAllowed(Pawn, this))
            {
                goto AfterConditionCheck;
            }
        }

        {
            AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
            if (PlayerChar && Pawn->IsPlayerControlled())
            {
                HasMetConditionsDoorSide(PlayerChar, bConditionsMet, FailReason, EDoorSide::None);
            }
            else
            {
                bConditionsMet = !bIsLockedForAI;
            }
        }
    }

AfterConditionCheck:
    // Notify DoorInteractor interface
    {
        UClass* PawnClass = Pawn->GetClass();
        if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
        {
            IDoorInteractor::Execute_OnOverlappedDoor(Pawn, bConditionsMet, FailReason, this);
        }
    }

    // Broadcast OnPawnEnteredDoor
    if (OnPawnEnteredDoor.IsBound())
    {
        OnPawnEnteredDoor.Broadcast(this, Pawn, bConditionsMet, FailReason);
    }

    bool bIsPlayer = Pawn->IsPlayerControlled();

    if (bConditionsMet)
    {
        if (bIsAutomaticDoorForPlayer && bIsPlayer)
        {
            // Disable blocker for player
            if (PlayerBlocker && IsValid(PlayerBlocker))
            {
                PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }
        }

        bool bAutoPlayer = bIsAutomaticDoorForPlayer && bIsPlayer;
        bool bAutoAI = bIsAutomaticDoorForAI && !bIsPlayer;

        if (!bIsOpen && (bAutoPlayer || bAutoAI))
        {
            bIsOpen = true;
            OnOpenDoor.Broadcast(Pawn->IsPlayerControlled());
        }
    }
    else
    {
        if (bIsPlayer)
        {
            // Enable blocker - player can't pass
            if (PlayerBlocker && IsValid(PlayerBlocker))
            {
                PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            }
        }
    }
}

void UDoorComponent::PawnExitedDoor(APawn* Pawn)
{
    if (!Pawn)
    {
        return;
    }

    bool bWasPlayerControlled = Pawn->IsPlayerControlled();

    PawnsInDoorway.Remove(Pawn);

    if (PawnsInDoorway.Num() == 0)
    {
        SetComponentTickEnabled(false);
    }

    // Broadcast pawn exited
    if (OnPawnExitedDoor.IsBound())
    {
        OnPawnExitedDoor.Broadcast(this, Pawn);
    }

    // Re-enable blocker if player left and auto-door is enabled
    if (bIsAutomaticDoorForPlayer && bWasPlayerControlled)
    {
        if (PlayerBlocker && IsValid(PlayerBlocker))
        {
            PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        }
    }

    // If door is open, check remaining pawns to decide if door should close
    if (bIsOpen)
    {
        bool bHasPlayerWithConditions = false;
        bool bHasAIWithConditions = false;

        for (auto It = PawnsInDoorway.CreateConstIterator(); It; ++It)
        {
            APawn* RemainingPawn = It->Key;
            if (!RemainingPawn || !IsValid(RemainingPawn))
            {
                continue;
            }

            bool bConditionsMet = false;

            {
                UClass* PawnClass = RemainingPawn->GetClass();
                if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
                {
                    if (IDoorInteractor::Execute_DoorEntryNotAllowed(RemainingPawn, this))
                    {
                        continue;
                    }
                }
            }

            {
                AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(RemainingPawn);
                if (PlayerChar && RemainingPawn->IsPlayerControlled())
                {
                    EConditionFailReason FailReason = EConditionFailReason::None;
                    HasMetConditionsDoorSide(PlayerChar, bConditionsMet, FailReason, EDoorSide::None);
                }
                else
                {
                    bConditionsMet = !bIsLockedForAI;
                }
            }

            if (bConditionsMet)
            {
                if (bWasPlayerControlled)
                {
                    bHasPlayerWithConditions = true;
                }
                else
                {
                    bHasAIWithConditions = true;
                }
            }
        }

        // Determine if door should close
        bool bBothAutoAndEmpty = bIsAutomaticDoorForPlayer && bIsAutomaticDoorForAI && !bHasPlayerWithConditions && !bHasAIWithConditions;
        bool bPlayerAutoOnly = bIsAutomaticDoorForPlayer && !bIsAutomaticDoorForAI && !bHasPlayerWithConditions && bWasPlayerControlled;
        bool bAIAutoOnly = !bIsAutomaticDoorForPlayer && bIsAutomaticDoorForAI && !bHasAIWithConditions && !bWasPlayerControlled;
        bool bCanCloseForAI = bCanCloseDoorForAI && !bHasAIWithConditions && !bWasPlayerControlled;

        if (bBothAutoAndEmpty || bPlayerAutoOnly || bAIAutoOnly || bCanCloseForAI)
        {
            UE_LOG(LogDoorComponent, Verbose, TEXT("CHECKING THE BOOLS"));
            bIsOpen = false;
            OnCloseDoor.Broadcast(Pawn->IsPlayerControlled());
        }
        else
        {
            UE_LOG(LogDoorComponent, Verbose, TEXT("NONE OF THE BOOLS WERE TRUE"));
        }
    }

    // Notify DoorInteractor of end overlap
    {
        UClass* PawnClass = Pawn->GetClass();
        if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
        {
            IDoorInteractor::Execute_OnEndOverlapDoor(Pawn);
        }
        else
        {
            UE_LOG(LogDoorComponent, Verbose, TEXT("WE CAN'T CALL ON END OVERLAP"));
        }
    }
}

bool UDoorComponent::IsLockedForPlayer() const
{
    return bIsLockedForPlayer;
}

bool UDoorComponent::IsLockedForAI() const
{
    return bIsLockedForAI;
}

void UDoorComponent::HasMetConditions(AFNAFBasePlayerCharacter* BasePlayerCharacter, bool& bConditionsMet, EConditionFailReason& FailReason)
{
    HasMetConditionsDoorSide(BasePlayerCharacter, bConditionsMet, FailReason, EDoorSide::None);
}

void UDoorComponent::HasMetConditionsDoorSide(AFNAFBasePlayerCharacter* BasePlayerCharacter, bool& bConditionsMet, EConditionFailReason& FailReason, EDoorSide Side)
{
    bConditionsMet = false;
    FailReason = EConditionFailReason::None;

    UWorld* World = GetWorld();
    if (!World) return;

    UFNAFGameInstanceBase* GI = Cast<UFNAFGameInstanceBase>(World->GetGameInstance());
    if (!GI) return;

    bool bIsChowda = (GI->GetCurrentGameType() == EFNAFGameType::ChowdaMode);
    const FDoorEntryRequirements& Reqs = bIsChowda ? ChowdaDoorEntryRequirements : NormalDoorEntryRequirements;

    if (BasePlayerCharacter)
    {
        // In Chowda mode, if door is already open, conditions are met
        if (bIsChowda && bIsOpen)
        {
            bConditionsMet = true;
            FailReason = EConditionFailReason::None;
            return;
        }

        // Check PawnIgnoresConditions - if pawn type is in list, conditions are automatically met
        EPlayerPawnType PawnType = BasePlayerCharacter->GetPlayerPawnType();
        for (const EPlayerPawnType& IgnoredType : Reqs.PawnIgnoresConditions)
        {
            if (IgnoredType == PawnType)
            {
                bConditionsMet = true;
                FailReason = EConditionFailReason::None;
                return;
            }
        }

        // Check ConditionDirection
        EDoorSide CondDir = Reqs.ConditionDirection;
        if (CondDir == EDoorSide::None)
        {
            // No direction restriction - conditions are met
            bConditionsMet = true;
            FailReason = EConditionFailReason::None;
            return;
        }

        if (CondDir != EDoorSide::Both)
        {
            // Check which side the pawn is on
            EDoorSide ActualSide = Side;
            if (Side == EDoorSide::Front)
            {
                ActualSide = GetDoorSideFromActor(BasePlayerCharacter);
                CondDir = Reqs.ConditionDirection;
            }

            if (ActualSide != CondDir)
            {
                // Pawn is on the other side - conditions apply to different side, so pass
                bConditionsMet = true;
                return;
            }
        }

        // Check RestrictedPawns - if pawn type is in list, it cannot enter
        EPlayerPawnType PawnType2 = BasePlayerCharacter->GetPlayerPawnType();
        for (const EPlayerPawnType& RestrictedType : Reqs.RestrictedPawns)
        {
            if (RestrictedType == PawnType2)
            {
                bConditionsMet = false;
                FailReason = EConditionFailReason::WrongPawn;
                return;
            }
        }

        // Check if door is locked for player
        if (bIsLockedForPlayer && BasePlayerCharacter->IsPlayerControlled())
        {
            bConditionsMet = false;
            return;
        }
    }

    // Check RequiredSecurityLevel (only in Normal mode)
    if (!bIsChowda && Reqs.RequiredSecurityLevel > 0)
    {
        UFNAFInventorySystem* InventorySystem = World->GetGameInstance()->GetSubsystem<UFNAFInventorySystem>();
        if (!InventorySystem)
        {
            bConditionsMet = false;
            FailReason = EConditionFailReason::Error;
            return;
        }

        if (InventorySystem->SecurityLevel < Reqs.RequiredSecurityLevel)
        {
            bConditionsMet = false;
            FailReason = EConditionFailReason::SecurityLevel;
            return;
        }
        bConditionsMet = true;
    }

    // Check RequiredInventoryItem
    if (Reqs.RequiredInventoryItem != NAME_None)
    {
        UFNAFInventorySystem* InventorySystem = World->GetGameInstance()->GetSubsystem<UFNAFInventorySystem>();
        if (!InventorySystem)
        {
            bConditionsMet = false;
            FailReason = EConditionFailReason::Error;
            return;
        }

        if (!InventorySystem->HasItem(Reqs.RequiredInventoryItem))
        {
            bConditionsMet = false;
            FailReason = EConditionFailReason::ItemRequired;
            return;
        }
    }

    bConditionsMet = true;
}

bool UDoorComponent::HasDoorInitialized() const
{
    return bDoorHasInitialized;
}

TArray<APawn*> UDoorComponent::GetPawnsInDoor() const
{
    TArray<APawn*> Result;
    PawnsInDoorway.GetKeys(Result);
    return Result;
}

bool UDoorComponent::GetIsAutomaticDoorEnabledForPlayer() const
{
    return bIsAutomaticDoorForPlayer;
}

bool UDoorComponent::GetIsAutomaticDoorEnabledForAI() const
{
    return bIsAutomaticDoorForAI;
}

EDoorSide UDoorComponent::GetDoorSideFromLocation(const FVector& WorldLocation) const
{
    AActor* Owner = GetOwner();
    if (!Owner) return EDoorSide::Front;

    FVector DoorLocation = FVector::ZeroVector;
    if (Owner->GetRootComponent())
    {
        DoorLocation = Owner->GetRootComponent()->GetComponentLocation();
    }

    FVector Direction = WorldLocation - DoorLocation;

    FVector Forward = FVector::ForwardVector;
    if (Owner->GetRootComponent())
    {
        Forward = Owner->GetRootComponent()->GetForwardVector();
    }

    float Dot = FVector::DotProduct(Direction, Forward);

    return (Dot <= 0.0f) ? EDoorSide::Back : EDoorSide::Front;
}

EDoorSide UDoorComponent::GetDoorSideFromActor(AActor* Actor) const
{
    if (!Actor)
    {
        return EDoorSide::None;
    }

    FVector ActorLocation = FVector::ZeroVector;
    if (Actor->GetRootComponent())
    {
        ActorLocation = Actor->GetRootComponent()->GetComponentLocation();
    }

    AActor* Owner = GetOwner();
    if (!Owner) return EDoorSide::Front;

    FVector DoorLocation = FVector::ZeroVector;
    USceneComponent* RootComp = Owner->GetRootComponent();
    if (RootComp)
    {
        DoorLocation = RootComp->GetComponentLocation();
    }

    FVector Direction = ActorLocation - DoorLocation;

    FVector Forward = FVector::ForwardVector;
    if (RootComp)
    {
        Forward = RootComp->GetForwardVector();
    }

    float Dot = FVector::DotProduct(Direction, Forward);

    return (Dot <= 0.0f) ? EDoorSide::Back : EDoorSide::Front;
}

FDoorEntryRequirements UDoorComponent::GetCurrentRequirements() const
{
    return GetActiveRequirements();
}

void UDoorComponent::ForceOpen()
{
    if (!bIsOpen)
    {
        bIsOpen = true;

        if (PlayerBlocker && IsValid(PlayerBlocker))
        {
            PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }

        OnOpenDoor.Broadcast(false);
    }
}

void UDoorComponent::ForceClose()
{
    if (bIsOpen)
    {
        bIsOpen = false;

        if (PlayerBlocker && IsValid(PlayerBlocker))
        {
            PlayerBlocker->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        }

        OnCloseDoor.Broadcast(false);
    }
}

void UDoorComponent::CopyConditions(UDoorComponent* OtherDoorComponent)
{
    if (OtherDoorComponent && IsValid(OtherDoorComponent))
    {
        ChowdaDoorEntryRequirements = OtherDoorComponent->ChowdaDoorEntryRequirements;
        NormalDoorEntryRequirements = OtherDoorComponent->NormalDoorEntryRequirements;
    }
}

void UDoorComponent::CanPawnOpenDoor(APawn* Pawn, bool& bOutConditionsMet, EConditionFailReason& OutFailReason)
{
    CanPawnOpenDoorSide(Pawn, bOutConditionsMet, OutFailReason, EDoorSide::None);
}

void UDoorComponent::CanPawnOpenDoorSide(APawn* Pawn, bool& bOutConditionsMet, EConditionFailReason& OutFailReason, EDoorSide Side)
{
    bOutConditionsMet = false;
    OutFailReason = EConditionFailReason::None;

    if (!Pawn || !IsValid(Pawn))
    {
        return;
    }

    // Check DoorInteractor interface
    UClass* PawnClass = Pawn->GetClass();
    if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
    {
        if (IDoorInteractor::Execute_DoorEntryNotAllowed(Pawn, this))
        {
            bOutConditionsMet = false;
            return;
        }
    }

    // If it's a player-controlled AFNAFBasePlayerCharacter, check full conditions
    AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
    if (PlayerChar && Pawn->IsPlayerControlled())
    {
        HasMetConditionsDoorSide(PlayerChar, bOutConditionsMet, OutFailReason, Side);
    }
    else
    {
        // For AI, just check AI lock state
        bOutConditionsMet = !bIsLockedForAI;
    }
}

bool UDoorComponent::CheckPawnConditions(APawn* Pawn, EDoorSide Side)
{
    if (!Pawn || !IsValid(Pawn))
    {
        return false;
    }

    bool bConditionsMet = false;
    EConditionFailReason FailReason = EConditionFailReason::None;

    UClass* PawnClass = Pawn->GetClass();
    if (PawnClass && PawnClass->ImplementsInterface(UDoorInteractor::StaticClass()))
    {
        if (IDoorInteractor::Execute_DoorEntryNotAllowed(Pawn, this))
        {
            return false;
        }
    }

    AFNAFBasePlayerCharacter* PlayerChar = Cast<AFNAFBasePlayerCharacter>(Pawn);
    if (PlayerChar && Pawn->IsPlayerControlled())
    {
        HasMetConditionsDoorSide(PlayerChar, bConditionsMet, FailReason, Side);
    }
    else
    {
        bConditionsMet = !bIsLockedForAI;
    }

    return bConditionsMet;
}