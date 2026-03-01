#include "FNAFSightSystem.h"
#include "SightComponent.h"
#include "VisualSourceComponent.h"
#include "MoonmanSpawnPoint.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"

UFNAFSightSystem::UFNAFSightSystem()
    : bSightDebugDisplayEnabled(true)
    , CurrentQueryID(0)
{
}

void UFNAFSightSystem::SetSightSystemDisplay(bool bEnable)
{
    bSightDebugDisplayEnabled = bEnable;
}

/* Components call these from BeginPlay/EndPlay to populate the registries that all queries iterate over. */

void UFNAFSightSystem::RegisterVisualSource(UVisualSourceComponent* Source)
{
    if (Source)
    {
        VisualSources.AddUnique(Source);
    }
}

void UFNAFSightSystem::UnregisterVisualSource(UVisualSourceComponent* Source)
{
    if (Source)
    {
        VisualSources.Remove(Source);
    }
}

void UFNAFSightSystem::RegisterMMSpawnPoint(AMoonmanSpawnPoint* SpawnPoint)
{
    if (SpawnPoint)
    {
        MMSpawnPoints.AddUnique(SpawnPoint);
    }
}

void UFNAFSightSystem::UnregisterMMSpawnPoint(AMoonmanSpawnPoint* SpawnPoint)
{
    if (SpawnPoint)
    {
        MMSpawnPoints.Remove(SpawnPoint);
    }
}

/* Removes all pending queries whose SightComponent matches. */
void UFNAFSightSystem::RemoveSightComponent(USightComponent* SightComp)
{
    TArray<uint32> KeysToRemove;
    for (auto& Pair : VisualSourceResultDelegateMap)
    {
        if (Pair.Value.IsValid() && Pair.Value->SightComponent.Get() == SightComp)
        {
            KeysToRemove.Add(Pair.Key);
        }
    }
    for (uint32 Key : KeysToRemove)
    {
        VisualSourceResultDelegateMap.Remove(Key);
    }

    KeysToRemove.Reset();
    for (auto& Pair : VisualSourceMoonManResultDelegateMap)
    {
        if (Pair.Value.IsValid() && Pair.Value->SightComponent.Get() == SightComp)
        {
            KeysToRemove.Add(Pair.Key);
        }
    }
    for (uint32 Key : KeysToRemove)
    {
        VisualSourceMoonManResultDelegateMap.Remove(Key);
    }
}

/*
 * Queues an async cone detection query. Starts a 10Hz processing timer if not
 * already running. Precomputes cosine of half-angle, snapshots the current
 * VisualSources registry, and stores the query keyed by a unique QueryID.
 */
void UFNAFSightSystem::DetectVisualSourcesInCone(
    USightComponent* SightComp,
    const FVector& Location,
    const FVector& Direction,
    float SightHalfAngle,
    float StartDistance,
    float EndDistance,
    const TArray<AActor*>& ActorsToIgnore,
    bool bDebug)
{
    if (!TimerHandle.IsValid())
    {
        UWorld* World = GetWorld();
        if (World)
        {
            FTimerManager& TimerManager = World->GetTimerManager();
            TimerManager.SetTimer(
                TimerHandle,
                this,
                &UFNAFSightSystem::OnTick,
                0.1f,
                true,
                -1.0f);
        }
    }

    float CosineHalfAngle = FMath::Cos(FMath::DegreesToRadians(SightHalfAngle));

    TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>> SourceQuery = MakeShared<FVisualSourceQuery<UVisualSourceComponent>>();

    SourceQuery->ComponentsToTest = VisualSources;
    SourceQuery->SightComponent = SightComp;
    SourceQuery->CosineSightHalfAngle = CosineHalfAngle;
    SourceQuery->StartDistance = StartDistance;
    SourceQuery->EndDistance = EndDistance;
    SourceQuery->Location = Location;
    SourceQuery->Forward = Direction;

    SourceQuery->Params = FCollisionQueryParams::DefaultQueryParam;
    SourceQuery->Params.bTraceComplex = false;
    SourceQuery->Params.AddIgnoredActors(ActorsToIgnore);

    uint32 QueryID = CurrentQueryID++;
    VisualSourceResultDelegateMap.Emplace(QueryID, SourceQuery);
}

/* Similar to DetectVisualSourcesInCone but for MoonMan spawn points. No collision params. */
void UFNAFSightSystem::DetectMMSpawnsInCone(
    USightComponent* SightComp,
    const FVector& Location,
    const FVector& Direction,
    float SightHalfAngle,
    float StartDistance,
    float EndDistance)
{
    float CosineHalfAngle = FMath::Cos(FMath::DegreesToRadians(SightHalfAngle));

    TSharedPtr<FVisualSourceQuery<AMoonmanSpawnPoint>> SourceQuery = MakeShared<FVisualSourceQuery<AMoonmanSpawnPoint>>();

    SourceQuery->ComponentsToTest = MMSpawnPoints;
    SourceQuery->SightComponent = SightComp;
    SourceQuery->CosineSightHalfAngle = CosineHalfAngle;
    SourceQuery->StartDistance = StartDistance;
    SourceQuery->EndDistance = EndDistance;
    SourceQuery->Location = Location;
    SourceQuery->Forward = Direction;

    uint32 QueryID = CurrentQueryID++;
    VisualSourceMoonManResultDelegateMap.Emplace(QueryID, SourceQuery);
}

/* Timer callback that processes pending async queries within a 5ms budget. */
void UFNAFSightSystem::OnTick()
{
    double StartTime = FPlatformTime::Seconds();
    double ElapsedTime = 0.0;

    while (VisualSourceResultDelegateMap.Num() > 0)
    {
        if (ElapsedTime >= 0.005)
        {
            break;
        }

        TArray<uint32> QueryIDs;
        VisualSourceResultDelegateMap.GetKeys(QueryIDs);
        if (QueryIDs.Num() > 0)
        {
            RunQuery(QueryIDs[0], false);
        }

        ElapsedTime = FPlatformTime::Seconds() - StartTime;
    }

    while (VisualSourceMoonManResultDelegateMap.Num() > 0)
    {
        if (ElapsedTime >= 0.005)
        {
            break;
        }

        TArray<uint32> QueryIDs;
        VisualSourceMoonManResultDelegateMap.GetKeys(QueryIDs);
        if (QueryIDs.Num() > 0)
        {
            RunMoonManQuery(QueryIDs[0]);
        }

        ElapsedTime = FPlatformTime::Seconds() - StartTime;
    }
}

/* Processes one visual source query incrementally, testing up to 16 components per call. */
void UFNAFSightSystem::RunQuery(uint32 QueryID, bool bWholeSet)
{
    TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>>* QueryPtr = VisualSourceResultDelegateMap.Find(QueryID);
    if (!QueryPtr || !QueryPtr->IsValid())
    {
        VisualSourceResultDelegateMap.Remove(QueryID);
        return;
    }

    TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>> Query = *QueryPtr;

    if (!Query->SightComponent.IsValid())
    {
        VisualSourceResultDelegateMap.Remove(QueryID);
        return;
    }

    int32 NumComponents = Query->ComponentsToTest.Num();

    if (Query->CurrentIndex >= NumComponents)
    {
        USightComponent* SightComp = Query->SightComponent.Get();
        if (SightComp)
        {
            SightComp->OnVisualQueryResults(Query->Results);
        }
        VisualSourceResultDelegateMap.Remove(QueryID);
        return;
    }

    UWorld* World = GetWorld();
    int32 Processed = 0;

    while (Query->CurrentIndex < NumComponents)
    {
        const TWeakObjectPtr<UVisualSourceComponent>& WeakSource = Query->ComponentsToTest[Query->CurrentIndex];

        if (TestVisualSource(WeakSource, Query, World))
        {
            Query->Results.Add(WeakSource);
        }

        Query->CurrentIndex++;

        if (++Processed >= 16)
        {
            break;
        }
    }

    if (Query->CurrentIndex >= NumComponents)
    {
        if (Query->SightComponent.IsValid())
        {
            USightComponent* SightComp = Query->SightComponent.Get();
            if (SightComp)
            {
                SightComp->OnVisualQueryResults(Query->Results);
            }
        }
        VisualSourceResultDelegateMap.Remove(QueryID);
    }
}

/*
 * Processes one MoonMan query incrementally. Does inline cone+distance+LOS
 * checks instead of calling TestVisualSource. Up to 16 per call.
 */
void UFNAFSightSystem::RunMoonManQuery(uint32 QueryID)
{
    TSharedPtr<FVisualSourceQuery<AMoonmanSpawnPoint>>* QueryPtr = VisualSourceMoonManResultDelegateMap.Find(QueryID);
    if (!QueryPtr || !QueryPtr->IsValid())
    {
        VisualSourceMoonManResultDelegateMap.Remove(QueryID);
        return;
    }

    TSharedPtr<FVisualSourceQuery<AMoonmanSpawnPoint>> Query = *QueryPtr;

    if (!Query->SightComponent.IsValid())
    {
        VisualSourceMoonManResultDelegateMap.Remove(QueryID);
        return;
    }

    int32 NumSpawnPoints = Query->ComponentsToTest.Num();

    if (Query->CurrentIndex >= NumSpawnPoints)
    {
        USightComponent* SightComp = Query->SightComponent.Get();
        if (SightComp)
        {
            SightComp->OnMoonManQueryResults(Query->Results);
        }
        VisualSourceMoonManResultDelegateMap.Remove(QueryID);
        return;
    }

    UWorld* World = GetWorld();
    int32 Processed = 0;

    while (Query->CurrentIndex < NumSpawnPoints)
    {
        const TWeakObjectPtr<AMoonmanSpawnPoint>& WeakSpawnPoint = Query->ComponentsToTest[Query->CurrentIndex];

        if (WeakSpawnPoint.IsValid())
        {
            AMoonmanSpawnPoint* SpawnPoint = WeakSpawnPoint.Get();

            FVector VisibleLocation = SpawnPoint->GetActorLocation();

            FVector ToTarget = VisibleLocation - Query->Location;
            float Distance = ToTarget.Size();

            FVector Direction;
            if (Distance > SMALL_NUMBER)
            {
                Direction = ToTarget / Distance;
            }
            else
            {
                Direction = FVector::ZeroVector;
            }

            if (Distance >= Query->StartDistance
                && Distance <= Query->EndDistance
                && FVector::DotProduct(Direction, Query->Forward) > Query->CosineSightHalfAngle)
            {
                FHitResult OutHit;
                bool bHit = World->LineTraceSingleByChannel(
                    OutHit,
                    Query->Location,
                    VisibleLocation,
                    ECC_Visibility,
                    FCollisionQueryParams::DefaultQueryParam,
                    FCollisionResponseParams::DefaultResponseParam);

                if (!bHit)
                {
                    Query->Results.Add(WeakSpawnPoint);
                }
            }
        }

        Query->CurrentIndex++;

        if (++Processed >= 16)
        {
            break;
        }
    }

    if (Query->CurrentIndex >= NumSpawnPoints)
    {
        if (Query->SightComponent.IsValid())
        {
            USightComponent* SightComp = Query->SightComponent.Get();
            if (SightComp)
            {
                SightComp->OnMoonManQueryResults(Query->Results);
            }
        }
        VisualSourceMoonManResultDelegateMap.Remove(QueryID);
    }
}

/*
 * Tests a single visual source against a query's cone + LOS. For each visual
 * location on the source, checks distance bounds, cone angle via dot product,
 * and line trace. If the trace hits the source's own actor, it counts as visible.
 */
bool UFNAFSightSystem::TestVisualSource(
    const TWeakObjectPtr<UVisualSourceComponent>& VisualSource,
    TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>> SourceQuery,
    UWorld* World)
{
    if (!VisualSource.IsValid())
    {
        return false;
    }

    UVisualSourceComponent* Source = VisualSource.Get();

    if (!Source->GetSourceVisibility())
    {
        return false;
    }

    TArray<FVector> Locations = Source->GetVisualLocations();

    AActor* SourceOwner = Source->GetOwner();

    for (const FVector& VisualLocation : Locations)
    {
        FVector ToTarget = VisualLocation - SourceQuery->Location;
        float Distance = ToTarget.Size();

        FVector Direction;
        if (Distance > SMALL_NUMBER)
        {
            Direction = ToTarget / Distance;
        }
        else
        {
            Direction = FVector::ZeroVector;
        }

        if (Distance >= SourceQuery->StartDistance
            && Distance <= SourceQuery->EndDistance
            && FVector::DotProduct(Direction, SourceQuery->Forward) > SourceQuery->CosineSightHalfAngle)
        {
            FHitResult OutHit;
            bool bHit = World->LineTraceSingleByChannel(
                OutHit,
                SourceQuery->Location,
                VisualLocation,
                ECC_Visibility,
                SourceQuery->Params,
                FCollisionResponseParams::DefaultResponseParam);

            if (!bHit)
            {
                return true;
            }

            if (OutHit.bBlockingHit)
            {
                AActor* HitActor = OutHit.GetActor();
                if (!HitActor || HitActor == SourceOwner)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

/*
 * Synchronous FOV-based detection. Transforms each visual location into
 * viewer-local space, then checks distance, yaw/pitch angle bounds, and LOS.
 * HalfPitch is derived from HalfFOV and aspect ratio: asin(tan(HalfFOV) / AspectRatio).
 */
void UFNAFSightSystem::DetectVisualSourcesInFov(
    const FTransform& ViewerTransform,
    float HalfFOV,
    float AspectRatio,
    float StartDistance,
    float EndDistance,
    const TArray<AActor*>& ActorsToIgnore,
    TArray<TWeakObjectPtr<UVisualSourceComponent>>& OutDetectedSources,
    bool bDebug)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    float HalfFOVRad = FMath::DegreesToRadians(HalfFOV);

    float TanOverAspect = FMath::Tan(HalfFOVRad) / AspectRatio;
    float ClampedTanOverAspect = FMath::Clamp(TanOverAspect, -1.0f, 1.0f);
    float HalfPitchRad = FMath::Asin(ClampedTanOverAspect);

    FVector ViewerLocation = ViewerTransform.GetLocation();

    for (const TWeakObjectPtr<UVisualSourceComponent>& WeakSource : VisualSources)
    {
        if (!WeakSource.IsValid())
        {
            continue;
        }

        UVisualSourceComponent* Source = WeakSource.Get();
        if (!Source->GetSourceVisibility())
        {
            continue;
        }

        TArray<FVector> Locations = Source->GetVisualLocations();

        FCollisionQueryParams Params(FCollisionQueryParams::DefaultQueryParam);
        Params.bTraceComplex = false;
        Params.AddIgnoredActors(ActorsToIgnore);

        AActor* SourceOwner = Source->GetOwner();

        for (const FVector& VisualLocation : Locations)
        {
            FVector LocalPos = ViewerTransform.InverseTransformPosition(VisualLocation);

            float Distance = LocalPos.Size();
            if (Distance < StartDistance || Distance > EndDistance)
            {
                continue;
            }

            float Yaw = FMath::Atan2(LocalPos.Y, LocalPos.X);
            float Pitch = FMath::Atan2(LocalPos.Z, LocalPos.X);

            if (FMath::Abs(Yaw) <= HalfFOVRad && FMath::Abs(Pitch) <= HalfPitchRad)
            {
                FHitResult OutHit;
                bool bHit = World->LineTraceSingleByChannel(
                    OutHit,
                    ViewerLocation,
                    VisualLocation,
                    ECC_Visibility,
                    Params,
                    FCollisionResponseParams::DefaultResponseParam);

                if (!bHit)
                {
                    OutDetectedSources.Add(WeakSource);
                    break;
                }
                else if (OutHit.bBlockingHit)
                {
                    AActor* HitActor = OutHit.GetActor();
                    if (!HitActor || HitActor == SourceOwner)
                    {
                        OutDetectedSources.Add(WeakSource);
                        break;
                    }
                }
            }
        }
    }
}

/*
 * Synchronous detection with explicit min/max yaw and pitch bounds.
 * Also supports IncludeTags filtering -- sources whose owner lacks a matching tag are skipped.
 */
void UFNAFSightSystem::DetectVisualSourcesInBothAxis(
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
    bool bDebug)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    float MinYawRad = FMath::DegreesToRadians(MinYaw);
    float MaxYawRad = FMath::DegreesToRadians(MaxYaw);
    float MinPitchRad = FMath::DegreesToRadians(MinPitch);
    float MaxPitchRad = FMath::DegreesToRadians(MaxPitch);

    FVector ViewerLocation = ViewerTransform.GetLocation();

    FCollisionQueryParams Params(FCollisionQueryParams::DefaultQueryParam);
    Params.bTraceComplex = false;
    Params.AddIgnoredActors(ActorsToIgnore);

    for (const TWeakObjectPtr<UVisualSourceComponent>& WeakSource : VisualSources)
    {
        if (!WeakSource.IsValid())
        {
            continue;
        }

        UVisualSourceComponent* Source = WeakSource.Get();

        if (IncludeTags.Num() > 0)
        {
            AActor* SourceOwner = Source->GetOwner();
            if (!SourceOwner)
            {
                continue;
            }

            bool bHasMatchingTag = false;
            for (const FName& Tag : IncludeTags)
            {
                if (SourceOwner->ActorHasTag(Tag))
                {
                    bHasMatchingTag = true;
                    break;
                }
            }

            if (!bHasMatchingTag)
            {
                continue;
            }
        }

        TArray<FVector> Locations = Source->GetVisualLocations();
        AActor* SourceOwner = Source->GetOwner();

        for (const FVector& VisualLocation : Locations)
        {
            FVector LocalPos = ViewerTransform.InverseTransformPosition(VisualLocation);

            float Distance = LocalPos.Size();
            if (Distance < StartDistance || Distance > EndDistance)
            {
                continue;
            }

            float Yaw = FMath::Atan2(LocalPos.Y, LocalPos.X);
            float Pitch = FMath::Atan2(LocalPos.Z, LocalPos.X);

            if (Yaw >= MinYawRad && Yaw <= MaxYawRad
                && Pitch >= MinPitchRad && Pitch <= MaxPitchRad)
            {
                FHitResult OutHit;
                bool bHit = World->LineTraceSingleByChannel(
                    OutHit,
                    ViewerLocation,
                    VisualLocation,
                    ECC_Visibility,
                    Params,
                    FCollisionResponseParams::DefaultResponseParam);

                if (!bHit)
                {
                    OutDetectedSources.Add(WeakSource);
                    break;
                }
                else if (OutHit.bBlockingHit)
                {
                    AActor* HitActor = OutHit.GetActor();
                    if (!HitActor || HitActor == SourceOwner)
                    {
                        OutDetectedSources.Add(WeakSource);
                        break;
                    }
                }
            }
        }
    }
}

/*
 * Checks if an actor is visible to a sight component by examining pending async
 * query state. First checks already-confirmed Results, then tests untested
 * ComponentsToTest entries immediately via TestVisualSource.
 */
bool UFNAFSightSystem::GetActorVisibility(USightComponent* SightComp, AActor* Actor)
{
    UWorld* World = GetWorld();

    for (auto& Pair : VisualSourceResultDelegateMap)
    {
        if (!Pair.Value.IsValid())
        {
            continue;
        }

        TSharedPtr<FVisualSourceQuery<UVisualSourceComponent>> Query = Pair.Value;

        if (Query->SightComponent.Get() != SightComp)
        {
            continue;
        }

        for (const TWeakObjectPtr<UVisualSourceComponent>& WeakResult : Query->Results)
        {
            if (WeakResult.IsValid())
            {
                UVisualSourceComponent* Source = WeakResult.Get();
                if (Source && Source->GetOwner() == Actor)
                {
                    return true;
                }
            }
        }

        for (const TWeakObjectPtr<UVisualSourceComponent>& WeakSource : Query->ComponentsToTest)
        {
            if (WeakSource.IsValid())
            {
                UVisualSourceComponent* Source = WeakSource.Get();
                if (Source && Source->GetOwner() == Actor)
                {
                    return TestVisualSource(WeakSource, Query, World);
                }
            }
        }
    }

    return false;
}
