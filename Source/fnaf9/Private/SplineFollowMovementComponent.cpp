/*
 * Moves the owning actor along a spline. Two modes:
 *   - bAutoMove: advances DistanceAlongSpline by MovementSpeed*DeltaTime each tick
 *   - Manual: external code sets DistanceAlongSpline, clamped with one-shot broadcast
 */

#include "SplineFollowMovementComponent.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"

USplineFollowMovementComponent::USplineFollowMovementComponent()
{
    PrimaryComponentTick.bCanEverTick = true;

    MovementSpeed = 0.0f;
    SplineToFollow = nullptr;
    bOrientToTangent = true;
    bForward = true;
    bAutoMove = true;
    DistanceAlongSpline = 0.0f;
    bIsFollowingSpline = false;
    bWasAlerted = true;
}

void USplineFollowMovementComponent::BeginPlay()
{
    Super::BeginPlay();
}

void USplineFollowMovementComponent::StopFollowingSpline()
{
    bIsFollowingSpline = false;
}

void USplineFollowMovementComponent::StartFollowingSpline()
{
    if (SplineToFollow)
    {
        bIsFollowingSpline = true;
    }
}

void USplineFollowMovementComponent::SetDistanceAlongSpline(float Distance)
{
    DistanceAlongSpline = Distance;
}

void USplineFollowMovementComponent::SetSplineToFollow(USplineComponent* Spline)
{
    SplineToFollow = Spline;

    if (bForward)
    {
        DistanceAlongSpline = 0.0f;
    }
    else
    {
        DistanceAlongSpline = Spline->GetSplineLength();
    }
}

void USplineFollowMovementComponent::SetDirectionAndSplineToFollow(USplineComponent* Spline, bool Direction)
{
    bForward = Direction;
    SplineToFollow = Spline;

    if (Direction)
    {
        DistanceAlongSpline = 0.0f;
    }
    else
    {
        DistanceAlongSpline = Spline->GetSplineLength();
    }
}

bool USplineFollowMovementComponent::IsFollowingSpline() const
{
    return bIsFollowingSpline;
}

float USplineFollowMovementComponent::GetCurrentDistance() const
{
    return DistanceAlongSpline;
}

//
//   1. Super::TickComponent
//   2. If no SplineToFollow or !bIsFollowingSpline → broadcast OnSplineLost
//   3. bAutoMove path:
//      - Advance distance by ±(DeltaTime * MovementSpeed)
//      - If reached end: clamp, stop following, broadcast OnFinished
//   4. Manual path (external distance control):
//      - Clamp to [0, SplineLength]
//      - If at bounds and !bWasAlerted: set bWasAlerted, broadcast OnFinished
//      - If in valid range and bWasAlerted: clear bWasAlerted
//   5. Get world location at distance
//   6. If bOrientToTangent: compute rotation from tangent + roll
//   7. SetActorLocation[AndRotation]
void USplineFollowMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    USplineComponent* Spline = SplineToFollow;

    if (!Spline || !bIsFollowingSpline)
    {
        if (OnSplineLost.IsBound())
        {
            OnSplineLost.Broadcast();
        }
        return;
    }

    const float SplineLength = Spline->GetSplineLength();

    if (bAutoMove)
    {
        const float Delta = DeltaTime * MovementSpeed;
        bool bReachedEnd = false;

        if (bForward)
        {
            DistanceAlongSpline += Delta;
            bReachedEnd = (DistanceAlongSpline >= SplineLength);
        }
        else
        {
            DistanceAlongSpline -= Delta;
            bReachedEnd = (DistanceAlongSpline <= 0.0f);
        }

        if (bReachedEnd)
        {
            //              otherwise (went past end), clamp to SplineLength
            if (DistanceAlongSpline < SplineLength)
            {
                DistanceAlongSpline = 0.0f;
            }
            else
            {
                DistanceAlongSpline = SplineLength;
            }

            bIsFollowingSpline = false;

            if (OnFinishedFollowingSpline.IsBound())
            {
                OnFinishedFollowingSpline.Broadcast();
            }
        }
    }
    else
    {
        if (DistanceAlongSpline >= SplineLength)
        {
            DistanceAlongSpline = SplineLength;
            // Fall through to alert check
        }
        else if (DistanceAlongSpline > 0.0f)
        {
            // In valid range — clear alert flag if set
            if (bWasAlerted)
            {
                bWasAlerted = false;
            }
            goto UpdatePosition;
        }
        else
        {
            DistanceAlongSpline = 0.0f;
            // Fall through to alert check
        }

        // At boundary — broadcast once
        if (!bWasAlerted)
        {
            bWasAlerted = true;

            if (OnFinishedFollowingSpline.IsBound())
            {
                OnFinishedFollowingSpline.Broadcast();
            }
        }
    }

UpdatePosition:
    const FVector WorldLocation = Spline->GetLocationAtDistanceAlongSpline(
        DistanceAlongSpline, ESplineCoordinateSpace::World);

    AActor* Owner = GetOwner();
    if (!Owner)
    {
        return;
    }

    if (bOrientToTangent)
    {
        const float Roll = Spline->GetRollAtDistanceAlongSpline(
            DistanceAlongSpline, ESplineCoordinateSpace::World);

        const FVector Tangent = Spline->GetTangentAtDistanceAlongSpline(
            DistanceAlongSpline, ESplineCoordinateSpace::World);

        const FRotator TangentRotation = FRotationMatrix::MakeFromX(Tangent).Rotator();
        const FRotator RollRotation(0.0f, 0.0f, Roll);

        const FQuat FinalQuat = TangentRotation.Quaternion() * RollRotation.Quaternion();
        const FRotator FinalRotation = FinalQuat.Rotator();

        Owner->SetActorLocationAndRotation(WorldLocation, FinalRotation, false, nullptr, ETeleportType::None);
    }
    else
    {
        Owner->SetActorLocation(WorldLocation, false, nullptr, ETeleportType::None);
    }
}