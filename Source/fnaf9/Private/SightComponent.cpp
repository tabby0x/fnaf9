#include "SightComponent.h"
#include "FNAFSightSystem.h"
#include "VisualSourceComponent.h"
#include "MoonmanSpawnPoint.h"
#include "Engine/World.h"

//
//   - *(_QWORD *)&this->SightAngle = 1110704128 → SightAngle=45.0f, StartDistance=0.0f
//   - PrimaryComponentTick byte |= 2 → bCanEverTick = true
//   - *(_WORD *)&this->bWaitingOnResults = 0 → two consecutive bools false
//   - *(_WORD *)&this->bSightDetectionEnabled = 1 → enabled=true, debug=false

USightComponent::USightComponent()
{
    SightAngle = 45.0f;
    StartDistance = 0.0f;

    PrimaryComponentTick.bCanEverTick = true;

    bWaitingOnResults = false;
    bWaitingOnMoonManResults = false;

    SightType = ESightType::Cone;

    EndDistance = 500.0f;
    AspectRatio = 1.875f;
    MinYaw = -45.0f;
    MaxYaw = 45.0f;
    MinPitch = -45.0f;
    MaxPitch = 45.0f;
    ThetaSteps = 10;
    PhiSteps = 15;

    bSightDetectionEnabled = true;
    bShowVisionDebug = false;
}

//

void USightComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UWorld* World = GetWorld();
    if (World)
    {
        UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();
        if (SightSystem)
        {
            SightSystem->RemoveSightComponent(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}

//
// Three-way branch on SightType:
//
//   Cone (0): Asynchronous.
//     - If !bWaitingOnResults: set flag, build ignore list (owner),
//       call SightSystem->DetectVisualSourcesInCone.
//     - If !bWaitingOnMoonManResults: set flag, call DetectMMSpawnsInCone.
//     - Results delivered later via OnVisualQueryResults/OnMoonManQueryResults.
//
//   FOV (1): Synchronous.
//     - Build ignore list, call SightSystem->DetectVisualSourcesInFov.
//     - SightSystem returns results directly; no further processing here.
//     - IDA: no bWaitingOnResults set, no result processing inline.
//
//   Frustum (2): Synchronous with inline result processing.
//     - Call SightSystem->DetectVisualSourcesInBothAxis.
//     - Resolve weak sources → owner actors, deduplicate.
//     - Full inline actor diff (ProcessDetectionResults).

void USightComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    FVector ComponentLocation = GetComponentLocation();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();

    switch (SightType)
    {
    case ESightType::Cone:
    {
        if (!bWaitingOnResults)
        {
            bWaitingOnResults = true;

            TArray<AActor*> IgnoreActors;
            IgnoreActors.Add(GetOwner());

            FVector Direction = GetForwardVector();

            if (SightSystem)
            {
                SightSystem->DetectVisualSourcesInCone(
                    this, ComponentLocation, Direction,
                    SightAngle, StartDistance, EndDistance,
                    IgnoreActors, bShowVisionDebug);
            }
        }

        if (!bWaitingOnMoonManResults)
        {
            bWaitingOnMoonManResults = true;

            FVector Direction = GetForwardVector();

            if (SightSystem)
            {
                SightSystem->DetectMMSpawnsInCone(
                    this, ComponentLocation, Direction,
                    SightAngle, StartDistance, EndDistance);
            }
        }
        break;
    }

    case ESightType::FOV:
    {
        TArray<AActor*> IgnoreActors;
        IgnoreActors.Add(GetOwner());

        TArray<TWeakObjectPtr<UVisualSourceComponent>> DetectedSources;

        if (SightSystem)
        {
            SightSystem->DetectVisualSourcesInFov(
                GetComponentTransform(),
                SightAngle, AspectRatio,
                StartDistance, EndDistance,
                IgnoreActors, DetectedSources,
                bShowVisionDebug);
        }
        break;
    }

    case ESightType::Frustum:
    {
        TArray<AActor*> IgnoreActors;
        IgnoreActors.Add(GetOwner());

        TArray<TWeakObjectPtr<UVisualSourceComponent>> DetectedSources;

        if (SightSystem)
        {
            SightSystem->DetectVisualSourcesInBothAxis(
                GetComponentTransform(),
                MinYaw, MaxYaw, MinPitch, MaxPitch,
                StartDistance, EndDistance,
                IncludeTags, IgnoreActors, DetectedSources,
                bShowVisionDebug);
        }

        TArray<AActor*> ActorsSpotted;
        for (const TWeakObjectPtr<UVisualSourceComponent>& WeakSource : DetectedSources)
        {
            if (WeakSource.IsValid())
            {
                UVisualSourceComponent* Source = WeakSource.Get();
                AActor* Owner = Source ? Source->GetOwner() : nullptr;
                if (Owner)
                {
                    ActorsSpotted.AddUnique(Owner);
                }
            }
        }

        // Full inline actor diff
        ProcessDetectionResults(ActorsSpotted);
        break;
    }
    }
}

// ProcessDetectionResults — Hidden helper restored from IDA
//
// Shared actor-diffing logic used by OnVisualQueryResults and TickComponent
// (Frustum mode). IDA shows identical code duplicated in both paths.
//
// Algorithm:
//   1. Copy VisibleActors → TempActorsVisible (previous frame's set)
//   2. For each actor in ActorsSpotted:
//      a. Remove from TempActorsVisible (still being seen)
//      b. If NOT in VisibleActors: new detection
//         → broadcast OnSightChanged(Actor, true)
//         → bind OnDestroyed → OnVisibleActorDestroyed
//      c. If already in VisibleActors: still visible
//         → update VisualSourceComponent with observer position
//   3. Remaining in TempActorsVisible: lost sight
//      → update VisualSourceComponent
//      → broadcast OnSightChanged(Actor, false)
//      → unbind OnDestroyed
//   4. Replace VisibleActors with ActorsSpotted

void USightComponent::ProcessDetectionResults(TArray<AActor*>& ActorsSpotted)
{
    FVector ComponentLocation = GetComponentLocation();

    TArray<AActor*> TempActorsVisible = VisibleActors;

    for (AActor* SpottedActor : ActorsSpotted)
    {
        TempActorsVisible.Remove(SpottedActor);

        if (!VisibleActors.Contains(SpottedActor))
        {
            OnSightChanged.Broadcast(SpottedActor, true);

            if (SpottedActor)
            {
                SpottedActor->OnDestroyed.AddDynamic(this, &USightComponent::OnVisibleActorDestroyed);
            }
        }
        else
        {
            NotifyVisualSource(SpottedActor, ComponentLocation, true);
        }
    }

    for (AActor* LostActor : TempActorsVisible)
    {
        if (LostActor && IsValid(LostActor))
        {
            NotifyVisualSource(LostActor, ComponentLocation, false);

            OnSightChanged.Broadcast(LostActor, false);

            LostActor->OnDestroyed.RemoveDynamic(this, &USightComponent::OnVisibleActorDestroyed);
        }
    }

    VisibleActors = ActorsSpotted;
}

// NotifyVisualSource — Hidden helper restored from IDA
//
//   1. GetComponentByClass(Actor, UVisualSourceComponent)
//   2. Cast check to UVisualSourceComponent
//   3. If owner has "Player" tag: call DetectTheSource(Location, Actor, bVisible)

void USightComponent::NotifyVisualSource(AActor* Actor, const FVector& ObserverLocation, bool bInVisible)
{
    if (!Actor)
    {
        return;
    }

    UVisualSourceComponent* VisualSource = Actor->FindComponentByClass<UVisualSourceComponent>();
    if (VisualSource)
    {
        AActor* Owner = GetOwner();
        if (Owner && Owner->ActorHasTag(FName("Player")))
        {
            VisualSource->DetectTheSource(ObserverLocation, Actor, bInVisible);
        }
    }
}

//
// Async callback from FNAFSightSystem when DetectVisualSourcesInCone completes.
//
// resolves TWeakObjectPtr<UVisualSourceComponent> → owner AActor*, deduplicates,
// then runs shared actor diff logic, clears bWaitingOnResults.

void USightComponent::OnVisualQueryResults(const TArray<TWeakObjectPtr<UVisualSourceComponent>>& DetectedSources)
{
    if (!IsValidLowLevel() || IsPendingKill())
    {
        return;
    }

    // Resolve weak pointers to owner actors, deduplicate
    TArray<AActor*> ActorsSpotted;
    for (const TWeakObjectPtr<UVisualSourceComponent>& WeakSource : DetectedSources)
    {
        if (WeakSource.IsValid())
        {
            UVisualSourceComponent* Source = WeakSource.Get();
            AActor* Owner = Source ? Source->GetOwner() : nullptr;
            if (Owner)
            {
                ActorsSpotted.AddUnique(Owner);
            }
        }
    }

    ProcessDetectionResults(ActorsSpotted);

    bWaitingOnResults = false;
}

//
// Async callback from FNAFSightSystem when DetectMMSpawnsInCone completes.
//
//   - Already tracked + still detected: broadcast OnMMDetected(true)
//   - New detection: silently add to VisibleMMActors
//   - Lost: broadcast OnMMDetected(false), remove from VisibleMMActors

void USightComponent::OnMoonManQueryResults(const TArray<TWeakObjectPtr<AMoonmanSpawnPoint>>& MoonManSpawnPoints)
{
    if (!IsValidLowLevel() || IsPendingKill())
    {
        return;
    }

    TArray<AMoonmanSpawnPoint*> TempMMActorsVisible = VisibleMMActors;

    // Resolve weak pointers
    TArray<AMoonmanSpawnPoint*> MMActorsSpotted;
    for (const TWeakObjectPtr<AMoonmanSpawnPoint>& WeakMM : MoonManSpawnPoints)
    {
        AMoonmanSpawnPoint* SpawnPoint = WeakMM.Get();
        if (SpawnPoint)
        {
            MMActorsSpotted.Add(SpawnPoint);
        }
    }

    // Process spotted MoonMen
    for (AMoonmanSpawnPoint* SpottedMM : MMActorsSpotted)
    {
        TempMMActorsVisible.Remove(SpottedMM);

        if (VisibleMMActors.Contains(SpottedMM))
        {
            if (SpottedMM)
            {
                SpottedMM->OnMMDetected.Broadcast(SpottedMM, true);
            }
        }
        else
        {
            VisibleMMActors.Add(SpottedMM);
        }
    }

    // Lost MoonMen
    for (AMoonmanSpawnPoint* LostMM : TempMMActorsVisible)
    {
        if (LostMM)
        {
            LostMM->OnMMDetected.Broadcast(LostMM, false);
        }
        VisibleMMActors.Remove(LostMM);
    }

    bWaitingOnMoonManResults = false;
}

void USightComponent::OnVisibleActorDestroyed(AActor* DestroyedActor)
{
    OnSightChanged.Broadcast(DestroyedActor, false);
    VisibleActors.Remove(DestroyedActor);
}

//

void USightComponent::SetSightEnabled(bool bEnable)
{
    PrimaryComponentTick.SetTickFunctionEnable(false);
    bSightDetectionEnabled = bEnable;
    PrimaryComponentTick.SetTickFunctionEnable(bEnable);

    if (!bEnable)
    {
        for (AActor* Actor : VisibleActors)
        {
            OnSightChanged.Broadcast(Actor, false);
        }
    }
}

//
// Two paths: if async results pending, queries SightSystem directly;
// otherwise checks local VisibleActors array.

bool USightComponent::IsActorVisible(AActor* Actor)
{
    if (bWaitingOnResults)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();
            if (SightSystem)
            {
                return SightSystem->GetActorVisibility(this, Actor);
            }
        }
    }

    return VisibleActors.Contains(Actor);
}

// Simple getters — direct member access

ESightType USightComponent::IsUsingConeSight() const { return SightType; }
bool USightComponent::IsSightEnabled() const { return bSightDetectionEnabled; }
TArray<AActor*> USightComponent::GetVisibleActors() const { return VisibleActors; }
float USightComponent::GetStartDistance() const { return StartDistance; }
float USightComponent::GetSightAngle() const { return SightAngle; }
float USightComponent::GetMinYaw() const { return MinYaw; }
float USightComponent::GetMinPitch() const { return MinPitch; }
float USightComponent::GetMaxYaw() const { return MaxYaw; }
float USightComponent::GetMaxPitch() const { return MaxPitch; }
TArray<FName> USightComponent::GetIncludeTags() const { return IncludeTags; }
float USightComponent::GetEndDistance() const { return EndDistance; }
float USightComponent::GetAspectRatio() const { return AspectRatio; }

void USightComponent::GetFrustumTypeParams(float& OutMinYaw, float& OutMaxYaw, float& OutMinPitch, float& OutMaxPitch) const
{
    OutMinYaw = MinYaw;
    OutMaxYaw = MaxYaw;
    OutMinPitch = MinPitch;
    OutMaxPitch = MaxPitch;
}

// Simple setters — direct member writes

void USightComponent::SetSightType(ESightType InSightType) { SightType = InSightType; }

void USightComponent::SetSightParams(float Angle, float MinDistance, float MaxDistance)
{
    SightAngle = Angle;
    StartDistance = MinDistance;
    EndDistance = MaxDistance;
}

void USightComponent::SetSightAngle(float Angle) { SightAngle = Angle; }
void USightComponent::SetIncludeTags(const TArray<FName>& InIgnoreTags) { IncludeTags = InIgnoreTags; }

void USightComponent::SetFrustumTypeParams(float InMinYaw, float InMaxYaw, float InMinPitch, float InMaxPitch)
{
    MaxPitch = InMaxPitch;
    MinYaw = InMinYaw;
    MaxYaw = InMaxYaw;
    MinPitch = InMinPitch;
}

void USightComponent::SetEndDistance(float Distance) { EndDistance = Distance; }
void USightComponent::SetAspectRatio(float Ratio) { AspectRatio = Ratio; }