/*
 * World subsystem managing all security cameras in the level.
 * Cameras self-register in BeginPlay and unregister in EndPlay.
 * Delegates broadcast camera alerts and trigger events to the Fazwatch/UI.
 */

#include "SecurityCameraSystem.h"
#include "SecurityCamera.h"

USecurityCameraSystem::USecurityCameraSystem()
{
}

void USecurityCameraSystem::PlayerSpotted(ASecurityCamera* SecurityCamera)
{
    OnCameraAlert.Broadcast(SecurityCamera);
}

TArray<ASecurityCamera*> USecurityCameraSystem::GetAllSecurityCameras() const
{
    return RegisteredCameras;
}