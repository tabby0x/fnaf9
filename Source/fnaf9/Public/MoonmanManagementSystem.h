#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EFNAFGameState.h"
#include "EMMAnimCategory.h"
#include "OnMMRegisterSpawnDelegate.h"
#include "OnMMUnregisterSpawnDelegate.h"
#include "MoonmanManagementSystem.generated.h"

class AMoonmanSpawnPoint;

UCLASS(Blueprintable)
class FNAF9_API UMoonmanManagementSystem : public UWorldSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnMMRegisterSpawn OnMMRegisterSpawn;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnMMUnregisterSpawn OnMMUnregisterSpawn;

    UMoonmanManagementSystem();

    UFUNCTION(BlueprintCallable)
    void UnRegisterSpawn(AMoonmanSpawnPoint* InSpawnPoint);

    UFUNCTION(BlueprintCallable)
    void UnpauseMoonmanManager();

    UFUNCTION(BlueprintCallable)
    void StartMoonmanLiteManager();

    UFUNCTION(BlueprintCallable)
    void StartMoonmanDangerManager();

    UFUNCTION(BlueprintCallable)
    void RegisterSpawn(AMoonmanSpawnPoint* InSpawnPoint);

    UFUNCTION(BlueprintCallable)
    void PauseMoonmanManager();

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<AMoonmanSpawnPoint*> GetAllMMSpawnPointsFor(EMMAnimCategory MMAnimation) const;

    UFUNCTION(BlueprintCallable)
    TArray<AMoonmanSpawnPoint*> GetAllMMSpawnPoints();

private:
    UFUNCTION()
    void OnWorldStateChanged(EFNAFGameState NewState, EFNAFGameState PrevState);

    // Core array of all registered moonman spawn points (discovered from IDA)
    UPROPERTY(Transient)
    TArray<AMoonmanSpawnPoint*> MMSpawnPoints;

    // Timer handle for the 5-second tick loop used by danger/lite managers
    FTimerHandle TimerHandle;

    // Populates MMSpawnPoints from actors already in the level via TActorIterator
    void FindAllSpawnPoints();

    // Binds to WorldStateSystem delegate and ensures GameClockSystem is initialized
    void StartManager();
};
