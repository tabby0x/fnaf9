#include "CameraHelperFunctions.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"

/* Computes horizontal and vertical angular deviation (in radians) from the
   player camera to a world location. Horizontal angle is computed by flattening
   both vectors onto XY and taking acos(dot). Vertical angle transforms into
   camera-local space and compares the XY-flattened vs full direction.
   If camera is invalid, both angles are set to FLT_MAX. */
void UCameraHelperFunctions::GetLocationAnglesFromCamera(
    const UObject* WorldContextObject,
    const FVector& Location,
    float& OutHorizontalAngle,
    float& OutVerticalAngle)
{
    APlayerCameraManager* CamMgr = UGameplayStatics::GetPlayerCameraManager(WorldContextObject, 0);
    if (!CamMgr || !CamMgr->IsValidLowLevel())
    {
        OutHorizontalAngle = FLT_MAX;
        OutVerticalAngle = FLT_MAX;
        return;
    }

    FVector CamLocation = CamMgr->GetCameraLocation();
    FRotator CamRotation = CamMgr->GetCameraRotation();

    // Transform direction into camera local space (for vertical angle)
    FQuat CamQuat = CamRotation.Quaternion();
    FQuat InvQuat = CamQuat.Inverse();
    FVector WorldDir = Location - CamLocation;
    FVector LocalDir = InvQuat.RotateVector(WorldDir);

    // Horizontal angle: flatten camera forward and target direction to XY, then acos(dot)
    FVector CamForward = CamRotation.RotateVector(FVector::ForwardVector);
    FVector CamForwardFlat(CamForward.X, CamForward.Y, 0.0f);
    CamForwardFlat = CamForwardFlat.GetSafeNormal();

    FVector CamLocation2 = CamMgr->GetCameraLocation();
    FVector ToTarget(Location.X - CamLocation2.X, Location.Y - CamLocation2.Y, 0.0f);
    FVector ToTargetFlat = ToTarget.GetSafeNormal();

    float HorizDot = FVector::DotProduct(CamForwardFlat, ToTargetFlat);
    HorizDot = FMath::Clamp(HorizDot, -1.0f, 1.0f);
    OutHorizontalAngle = FMath::Acos(HorizDot);

    // Vertical angle: compare XY-flattened local dir vs full local dir
    FVector LocalDirFlat(LocalDir.X, LocalDir.Y, 0.0f);
    FVector LocalDirFlatNorm = LocalDirFlat.GetSafeNormal();
    FVector LocalDirFullNorm = LocalDir.GetSafeNormal();

    float VertDot = FVector::DotProduct(LocalDirFlatNorm, LocalDirFullNorm);
    VertDot = FMath::Clamp(VertDot, -1.0f, 1.0f);
    OutVerticalAngle = FMath::Acos(VertDot);
}

/* Checks if a world location falls within the player camera's FOV.
   Computes half-FOV in radians for both axes, then checks angular deviation
   against the requested CameraAngles flags (bit 0 = horizontal, bit 1 = vertical). */
bool UCameraHelperFunctions::IsLocationInCameraView(
    const UObject* WorldContextObject,
    const FVector& Location,
    ECameraAngleFlags CameraAngles,
    float& OutHorizontalAngle,
    float& OutVerticalAngle)
{
    APlayerCameraManager* CamMgr = UGameplayStatics::GetPlayerCameraManager(WorldContextObject, 0);
    if (!CamMgr || !CamMgr->IsValidLowLevel())
    {
        return false;
    }

    // Half horizontal FOV in radians
    float FOVDegrees = CamMgr->GetFOVAngle();
    float HalfHFOV = (FOVDegrees * 0.5f) * 0.017453292f; // PI / 180

    // Half vertical FOV: asin(tan(halfHFOV) / aspectRatio)
    float TanHalfH = FMath::Tan(HalfHFOV);
    float VRatio = TanHalfH / CamMgr->DefaultAspectRatio;
    VRatio = FMath::Clamp(VRatio, -1.0f, 1.0f);
    float HalfVFOV = FMath::Asin(VRatio);

    GetLocationAnglesFromCamera(WorldContextObject, Location, OutHorizontalAngle, OutVerticalAngle);

    uint8 Flags = static_cast<uint8>(CameraAngles);
    bool bHorizOk = (Flags & 1) == 0 || HalfHFOV >= OutHorizontalAngle;
    bool bVertOk = (Flags & 2) == 0 || HalfVFOV >= OutVerticalAngle;

    return bHorizOk && bVertOk;
}

UCameraHelperFunctions::UCameraHelperFunctions()
{
}
