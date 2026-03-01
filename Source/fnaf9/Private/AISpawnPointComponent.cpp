/* Arrow component used to mark AI spawn locations in the editor.
   Constructor disables tick. BeginPlay touches the AIManagementSystem
   subsystem to ensure it's initialized. */

#include "AISpawnPointComponent.h"
#include "AIManagementSystem.h"

UAISpawnPointComponent::UAISpawnPointComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UAISpawnPointComponent::BeginPlay()
{
    Super::BeginPlay();

    UWorld* World = GetWorld();
    if (World)
    {
        // Forces the AIManagementSystem subsystem to exist/initialize
        World->GetSubsystem<UAIManagementSystem>();
    }
}
