#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"
#include "FNAFSightSystem.generated.h"

class AActor;
class USightComponent;
class UVisualSourceComponent;
class AMoonmanSpawnPoint;

/*
 * Internal query struct for async sight detection. Allocated as 0xD8 (216) bytes
 * in IDA. Wrapped in TSharedPtr and stored in query maps keyed by QueryID.
 */
template<typename T>
struct FVisualSourceQuery
{
    TWeakObjectPtr<USightComponent> SightComponent;
    TArray<TWeakObjectPtr<T>> ComponentsToTest;
    TArray<TWeakObjectPtr<T>> Results;
    int32 CurrentIndex;
    float StartDistance;
    float EndDistance;
    float CosineSightHalfAngle;
    FVector Location;
    FVector Forward;
    FCollisionQueryParams Params;

    FVisualSourceQuery()
        : CurrentIndex(0)
        , StartDistance(0.0f)
        , EndDistance(0.0f)
        , CosineSightHalfAngle(0.0f)
        , Location(FVector::ZeroVector)
        , Forward(FVector::ZeroVector)
    {
    }
};

UCLASS(Blueprintable)
class FNAF9_API UFNAFSightSystem : public UWorldSubsystem {
    GENERATED_BODY()
public:
    UFNAFSightSystem();

    UFUNCTION(BlueprintCallable, Exec)
    void SetSightSystemDisplay(bool bEnable);

    /* Async detection -- queue queries processed by timer */

    void DetectVisualSourcesInCone(
        USightComponent* SightComp,
        const FVector& Location,
        const FVector& Direction,
        float SightHalfAngle,
        float StartDistance,
        float EndDistance,
        const TArray<AActor*>& ActorsToIgnore,
        bool bDebug = false);

    void DetectMMSpawnsInCone(
        USightComponent* SightComp,
        const FVector& Location,
        const FVector& Direction,
        float SightHalfAngle,
        float StartDistance,
        float EndDistance);

    /* Synchronous detection -- results returned immediately */

    void DetectVisualSourcesInFov(
        const FTransform& ViewerTransform,
        float HalfFOV,
        float AspectRatio,
        float StartDistance,
        float EndDistance,
        const TArray<AActor*>& ActorsToIgnore,
        TArray<TWeakObjectPtr<UVisualSourceComponent>>& OutDetectedSources,
        bool bDebug = false);

    void DetectVisualSourcesInBothAxis(
        const FTransform& ViewerTransform,
        float MinYaw,
        float MaxYaw,
        float MinPitch,
        float MaxPitch,
        float StartDistance,
        float EndDistance,
        const TArray<FName>& IncludeTags,
        const TArray<AActor*>& ActorsToIgnore,
        TArray<TWeakObjectPtr<UVisualSourceComponent>>& OutDetectedSources,
        bool bDebug = false);

    /* Queries & Registration */

    bool GetActorVisibility(USightComponent* SightComp, AActor* Actor);

    void RegisterVisualSource(UVisualSourceComponent* Source);
    void UnregisterVisualSource(UVisualSourceComponent* Source);
    void RegisterMMSpawnPoint(AMoonmanSpawnPoint* SpawnPoint);
    void UnregisterMMSpawnPoint(AMoonmanSpawnPoint* SpawnPoint);
    void RemoveSightComponent(USightComponent* SightComp);

private:
    /** Timer callback. Processes pending queries within 5ms budget. */
    void OnTick();

    /** Process one visual source query incrementally (up to 16 components). */
    void RunQuery(uint32 QueryID, bool bWholeSet = false);

    /** Process one MoonMan query incrementally (up to 16 spawn points). */
    void RunMoonManQuery(uint32 QueryID);

    /** Test a single visual source against query cone + LOS. */
    bool TestVisualSource(
        const TWeakObjectPtr<UVisualSourceComponent>& VisualSource,
        TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>> SourceQuery,
        UWorld* World);

    TMap<uint32, TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>>> VisualSourceResultDelegateMap;
    TMap<uint32, TSharedPtr<FVisualSourceQuery<AMoonmanSpawnPoint>>> VisualSourceMoonManResultDelegateMap;
    TArray<TWeakObjectPtr<UVisualSourceComponent>> VisualSources;
    TArray<TWeakObjectPtr<AMoonmanSpawnPoint>> MMSpawnPoints;
    bool bSightDebugDisplayEnabled;
    uint32 CurrentQueryID;
    FTimerHandle TimerHandle;
};
