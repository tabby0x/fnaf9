#include "UVLightReactionComponent.h"
#include "UVEmitterInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/Actor.h"

//
//   *(_WORD *)&this->EmitterInRange = 0  → zeroes EmitterInRange + OverrideUVLightPrecision
//   v3 | 2 → bCanEverTick = true (opposite of VisualSourceComponent)

UUVLightReactionComponent::UUVLightReactionComponent()
{
    DistanceExponent = 4.0f;
    ProjectExponent = 1.0f;
    Radius = 512.0f;
    EmitterInRange = false;
    OverrideUVLightPrecision = false;
    OverrideUVLightPrecisionValue = 0.0f;
    Material = nullptr;

    PrimaryComponentTick.bCanEverTick = true;
}

//

void UUVLightReactionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    CalculateUV();
}

//

void UUVLightReactionComponent::Setup(UMaterialInstanceDynamic* InMaterial, TArray<USceneComponent*> InLocatorComponents)
{
    Material = InMaterial;

    for (int32 i = 0; i < InLocatorComponents.Num(); ++i)
    {
        USceneComponent* Comp = InLocatorComponents[i];
        if (Comp)
        {
            LocatorComponents.Add(Comp);
        }
    }

    CalculateUV();
}

//

void UUVLightReactionComponent::AddActor(AActor* ActorToAdd)
{
    if (!ActorToAdd)
    {
        return;
    }

    if (!ActorToAdd->GetClass()->ImplementsInterface(UUVEmitterInterface::StaticClass()))
    {
        return;
    }

    TWeakObjectPtr<AActor> WeakActor(ActorToAdd);
    bool bFound = false;
    for (int32 i = 0; i < Actors.Num(); ++i)
    {
        if (Actors[i] == WeakActor)
        {
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        Actors.Add(WeakActor);
    }

    SetComponentTickEnabled(true);
}

//

void UUVLightReactionComponent::RemoveActor(AActor* ActorToRemove)
{
    TWeakObjectPtr<AActor> WeakActor(ActorToRemove);
    for (int32 i = Actors.Num() - 1; i >= 0; --i)
    {
        if (Actors[i] == WeakActor)
        {
            Actors.RemoveAt(i);
        }
    }

    if (Actors.Num() == 0)
    {
        SetComponentTickEnabled(false);
    }
}

float UUVLightReactionComponent::GetRadius() const
{
    return Radius;
}

bool UUVLightReactionComponent::GetEmitterInRange() const
{
    return EmitterInRange;
}

//
// This is the core UV light projection algorithm. For each tracked UV emitter
// actor, it:
//   1. Gets the emitter's world location (root component)
//   2. Finds the closest locator component on this actor
//   3. Calls IUVEmitterInterface::GetUVLight to get light location + strength
//   4. Calculates distance-based attenuation: pow(1 - dist/Radius, DistanceExponent)
//   5. Accumulates weighted direction vectors
//
// After processing all actors, it projects the accumulated direction into
// the owner's local 2D coordinate space (right/up vectors) for use as
// material UV coordinates:
//   - Material param "X" = horizontal projection + 0.5
//   - Material param "Y" = vertical projection + 0.5
//   - Material param "Intensity" = accumulated attenuation value
//
// The 0.5 offset centers the UV coordinates so (0.5, 0.5) = no light.

void UUVLightReactionComponent::CalculateUV()
{
    if (LocatorComponents.Num() <= 0)
    {
        EmitterInRange = false;
        return;
    }

    float Value = 0.0f;
    float AccumX = 0.0f;
    float AccumY = 0.0f;
    float AccumZ = 0.0f;

    for (int32 ActorIdx = 0; ActorIdx < Actors.Num(); ++ActorIdx)
    {
        if (!Actors[ActorIdx].IsValid())
        {
            EmitterInRange = false;
            continue;
        }

        AActor* EmitterActor = Actors[ActorIdx].Get();

        FVector EmitterLocation = FVector::ZeroVector;
        USceneComponent* EmitterRoot = EmitterActor->GetRootComponent();
        if (EmitterRoot)
        {
            EmitterLocation = EmitterRoot->GetComponentTransform().GetTranslation();
        }

        float ClosestDist = -1.0f;
        TWeakObjectPtr<USceneComponent> ClosestLocator;

        for (int32 LocIdx = 0; LocIdx < LocatorComponents.Num(); ++LocIdx)
        {
            if (!LocatorComponents[LocIdx].IsValid())
            {
                continue;
            }

            USceneComponent* LocComp = LocatorComponents[LocIdx].Get();
            FVector LocLocation = LocComp->GetComponentTransform().GetTranslation();
            float Dist = FVector::Dist(LocLocation, EmitterLocation);

            if (ClosestDist < 0.0f || Dist < ClosestDist)
            {
                ClosestDist = Dist;
                ClosestLocator = LocComp;
            }
        }

        if (!ClosestLocator.IsValid())
        {
            EmitterInRange = false;
            return;
        }

        FVector TargetLocation = ClosestLocator->GetComponentTransform().GetTranslation();

        FVector LightLocation = FVector::ZeroVector;
        float UVStrength = 0.0f;
        IUVEmitterInterface::Execute_GetUVLight(
            EmitterActor,
            TargetLocation,
            OverrideUVLightPrecision,
            OverrideUVLightPrecisionValue,
            LightLocation,
            UVStrength);

        FVector Dir = LightLocation - TargetLocation;
        float DistSq = Dir.SizeSquared();
        float Distance = FMath::Sqrt(DistSq);

        float Attenuation = FMath::Pow(1.0f - (Distance / Radius), DistanceExponent);
        Value += Attenuation * UVStrength;

        FVector NormalizedDir;
        if (DistSq == 1.0f)
        {
            NormalizedDir = Dir;
        }
        else if (DistSq >= 1e-8f)
        {
            float InvLen = 1.0f / FMath::Sqrt(DistSq);
            float HalfDistSq = DistSq * 0.5f;
            InvLen = InvLen + (InvLen * (0.5f - (HalfDistSq * (InvLen * InvLen))));
            InvLen = InvLen + (InvLen * (0.5f - (HalfDistSq * (InvLen * InvLen))));
            NormalizedDir = Dir * InvLen;
        }
        else
        {
            NormalizedDir = FVector::ZeroVector;
        }

        float Weight = Radius - Distance;
        AccumX += Weight * NormalizedDir.X;
        AccumY += Weight * NormalizedDir.Y;
        AccumZ += Weight * NormalizedDir.Z;
    }

    AActor* Owner = GetOwner();
    if (!Owner)
    {
        return;
    }

    FVector Forward;
    USceneComponent* RootComp = Owner->GetRootComponent();
    if (RootComp)
    {
        Forward = RootComp->GetForwardVector();
    }
    else
    {
        Forward = FVector::ForwardVector;
    }

    float ForwardProjection = (AccumX * Forward.X) + (AccumY * Forward.Y) + (AccumZ * Forward.Z);

    float PerpX = AccumX - (Forward.X * ForwardProjection);
    float PerpY = AccumY - (Forward.Y * ForwardProjection);
    float PerpZ = AccumZ - (Forward.Z * ForwardProjection);

    float PerpLength = FMath::Sqrt((PerpX * PerpX) + (PerpY * PerpY) + (PerpZ * PerpZ));

    float NormPerpX, NormPerpY, NormPerpZ;
    if (PerpLength <= 1e-8f)
    {
        NormPerpX = 0.0f;
        NormPerpY = 0.0f;
        NormPerpZ = 0.0f;
    }
    else
    {
        float InvPerp = 1.0f / PerpLength;
        NormPerpX = PerpX * InvPerp;
        NormPerpY = PerpY * InvPerp;
        NormPerpZ = PerpZ * InvPerp;
    }

    FVector Up;
    if (RootComp)
    {
        Up = RootComp->GetUpVector();
    }
    else
    {
        Up = FVector::UpVector;
    }

    float UpDot = (Up.X * NormPerpX) + (Up.Y * NormPerpY) + (Up.Z * NormPerpZ);

    FVector Right;
    if (RootComp)
    {
        Right = RootComp->GetRightVector();
    }
    else
    {
        Right = FVector::RightVector;
    }

    float RightDot = (Right.X * NormPerpX) + (Right.Y * NormPerpY) + (Right.Z * NormPerpZ);

    float VerticalComponent = -(UpDot * PerpLength);
    float HorizontalComponent = RightDot * PerpLength;

    float TotalLength = FMath::Sqrt((AccumX * AccumX) + (AccumY * AccumY) + (AccumZ * AccumZ));
    float ProjectionMag = FMath::Pow(TotalLength / Radius, ProjectExponent);

    float Len2D_Sq = (HorizontalComponent * HorizontalComponent) + (VerticalComponent * VerticalComponent);
    float NormHoriz, NormVert;

    if (Len2D_Sq <= 1e-8f)
    {
        NormHoriz = 0.0f;
        NormVert = 0.0f;
    }
    else
    {
        float InvLen = 1.0f / FMath::Sqrt(Len2D_Sq);
        float HalfLen2D = Len2D_Sq * 0.5f;
        InvLen = InvLen + (InvLen * (0.5f - (HalfLen2D * (InvLen * InvLen))));
        InvLen = InvLen + (InvLen * (0.5f - (HalfLen2D * (InvLen * InvLen))));
        NormHoriz = HorizontalComponent * InvLen;
        NormVert = VerticalComponent * InvLen;
    }

    if (Material)
    {
        Material->SetScalarParameterValue(FName(TEXT("X")), (ProjectionMag * NormVert) + 0.5f);
        Material->SetScalarParameterValue(FName(TEXT("Y")), (ProjectionMag * NormHoriz) + 0.5f);
        Material->SetScalarParameterValue(FName(TEXT("Intensity")), Value);
    }

    EmitterInRange = Value > 0.0f;
}