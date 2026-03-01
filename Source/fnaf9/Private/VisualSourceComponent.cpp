#include "VisualSourceComponent.h"
#include "FNAFSightSystem.h"
#include "Engine/World.h"

UVisualSourceComponent::UVisualSourceComponent()
{
    bIsVisibilityEnabled = true;
    DetectedVisualLocation = FVector::ZeroVector;
    PrimaryComponentTick.bCanEverTick = false;
}

/* Registration must happen BEFORE Super::BeginPlay -- without it the
   SightSystem has zero visual sources and AI sight detection is dead. */
void UVisualSourceComponent::BeginPlay()
{
    UWorld* World = GetWorld();
    if (World)
    {
        UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();
        if (SightSystem)
        {
            SightSystem->RegisterVisualSource(this);
        }
    }

    Super::BeginPlay();
}

void UVisualSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    UWorld* World = GetWorld();
    if (World)
    {
        UFNAFSightSystem* SightSystem = World->GetSubsystem<UFNAFSightSystem>();
        if (SightSystem)
        {
            SightSystem->UnregisterVisualSource(this);
        }
    }
}

void UVisualSourceComponent::SetVisualOffsetLocations(const TArray<FVector>& InVisualOffsets)
{
    if (&VisualOffsets != &InVisualOffsets)
    {
        VisualOffsets = InVisualOffsets;
    }
}

void UVisualSourceComponent::SetVisualOffset(int32 PointIndex, FVector visualOffset)
{
    // No bounds check in original
    VisualOffsets[PointIndex] = visualOffset;
}

void UVisualSourceComponent::SetSourceVisibility(bool bEnable)
{
    bIsVisibilityEnabled = bEnable;
}

void UVisualSourceComponent::RemoveVisualOffset(int32 PointIndex)
{
    // No bounds check in original
    VisualOffsets.RemoveAt(PointIndex);
}

TArray<FVector> UVisualSourceComponent::GetVisualOffsets() const
{
    return VisualOffsets;
}

/* Transforms VisualOffsets from local to world space. Falls back to
   a single-element array of the owner's root component location if empty. */
TArray<FVector> UVisualSourceComponent::GetVisualLocations() const
{
    TArray<FVector> Locations;

    AActor* Owner = GetOwner();
    if (!Owner)
    {
        return Locations;
    }

    if (VisualOffsets.Num() <= 0)
    {
        USceneComponent* RootComp = Owner->GetRootComponent();
        if (RootComp)
        {
            Locations.Add(RootComp->GetComponentTransform().GetTranslation());
        }
        else
        {
            Locations.Add(FVector::ZeroVector);
        }
        return Locations;
    }

    Locations = VisualOffsets;

    USceneComponent* RootComp = Owner->GetRootComponent();
    FTransform Transform = RootComp ? RootComp->GetComponentTransform() : FTransform::Identity;

    for (int32 i = 0; i < Locations.Num(); ++i)
    {
        Locations[i] = Transform.TransformPosition(Locations[i]);
    }

    return Locations;
}

bool UVisualSourceComponent::GetSourceVisibility() const
{
    return bIsVisibilityEnabled;
}

FVector UVisualSourceComponent::GetDetectedVisualLocation() const
{
    return DetectedVisualLocation;
}

void UVisualSourceComponent::DetectTheSource(const FVector VSLocation, AActor* passed_HitActor, bool passed_bVisible)
{
    DetectedVisualLocation = VSLocation;
    OnSourceDetected.Broadcast(this, passed_HitActor, passed_bVisible);
}