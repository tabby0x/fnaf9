#include "UVLightReactionFromOverlap.h"
#include "UVEmitterInterface.h"
#include "GameFramework/Actor.h"

UUVLightReactionFromOverlap::UUVLightReactionFromOverlap()
{
}

void UUVLightReactionFromOverlap::BeginPlay()
{
    Super::BeginPlay();

    AActor* Owner = GetOwner();
    if (Owner)
    {
        Owner->OnActorBeginOverlap.AddDynamic(this, &UUVLightReactionFromOverlap::OnOwnerBeginOverlap);

        Owner->OnActorEndOverlap.AddDynamic(this, &UUVLightReactionFromOverlap::OnOwnerEndOverlap);
    }
}

void UUVLightReactionFromOverlap::OnOwnerBeginOverlap(AActor* OverlappedActor, AActor* OtherActor)
{
    if (!OtherActor)
    {
        return;
    }

    if (!OtherActor->GetClass()->ImplementsInterface(UUVEmitterInterface::StaticClass()))
    {
        return;
    }

    TWeakObjectPtr<AActor> WeakActor(OtherActor);
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

void UUVLightReactionFromOverlap::OnOwnerEndOverlap(AActor* OverlappedActor, AActor* OtherActor)
{
    TWeakObjectPtr<AActor> WeakActor(OtherActor);
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