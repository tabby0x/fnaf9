#include "AudioUtil.h"
#include "AkAudioDevice.h"
#include "AkComponent.h"
#include "AkGameObject.h"

UAudioUtil::UAudioUtil() {}

void UAudioUtil::AddWwiseListener(AActor* Actor)
{
    if (!Actor) return;

    UAkComponent* AkComp = Actor->FindComponentByClass<UAkComponent>();
    if (!AkComp) return;

    FAkAudioDevice* AudioDevice = FAkAudioDevice::Get();
    if (AudioDevice)
    {
        AudioDevice->AddDefaultListener(AkComp);
    }
}

void UAudioUtil::RemoveWwiseListener(AActor* Actor)
{
    if (!Actor) return;

    UAkComponent* AkComp = Actor->FindComponentByClass<UAkComponent>();
    if (!AkComp) return;

    FAkAudioDevice* AudioDevice = FAkAudioDevice::Get();
    if (AudioDevice)
    {
        AudioDevice->RemoveDefaultListener(AkComp);
    }
}

TSet<UAkComponent*> UAudioUtil::GetWwiseListeners()
{
    FAkAudioDevice* AudioDevice = FAkAudioDevice::Get();
    if (AudioDevice)
    {
        return AudioDevice->GetDefaultListeners();
    }
    return TSet<UAkComponent*>();
}

bool UAudioUtil::IsAudioForActorPlaying(const UObject* WorldContextObject, AActor* Actor)
{
    if (!Actor) return false;

    FAkAudioDevice* AudioDevice = FAkAudioDevice::Get();
    if (!AudioDevice) return false;

    UAkComponent* AkComp = Actor->FindComponentByClass<UAkComponent>();
    if (!AkComp) return false;

    return AkComp->HasActiveEvents();
}
