#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AISpawnSystem.generated.h"

class APawn;
class UAIManagementSystem;

UCLASS(Blueprintable)
class FNAF9_API AAISpawnSystem : public AActor {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<APawn*> ExistingAI;

    AAISpawnSystem();
    virtual void BeginPlay() override;

private:
    UFUNCTION(BlueprintCallable)
    void OnRollChange();

    // IDA: called from BeginPlay and OnRollChange
    void AddAIToPath();

    // IDA: hidden members from constructor
    TArray<TWeakObjectPtr<UObject>> SpawnPoints;

    UPROPERTY(Transient)
    UAIManagementSystem* AIManagementSystem = nullptr;
};