#include "RoomSystem.h"
#include "RoomAreaBase.h"
#include "RoomSystemSettings.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

URoomSystem::URoomSystem()
{
    RoomUpdateTick = 0.5f;
    bPOIDetectionVisibility = false;
    bPOIVisibility = false;
}

void URoomSystem::StartRoomSystem()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FTimerManager& TimerManager = World->GetTimerManager();
    FTimerDelegate TimerDelegate;
    TimerDelegate.BindUObject(this, &URoomSystem::RoomSystemTick);
    TimerManager.SetTimer(TimerHandle, TimerDelegate, RoomUpdateTick, true);
}

void URoomSystem::RegisterRoom(ARoomAreaBase* Room)
{
    AllLoadedRooms.AddUnique(Room);

    if (OnRoomLoaded.IsBound())
    {
        OnRoomLoaded.Broadcast(Room);
    }
}

void URoomSystem::UnregisterRoom(ARoomAreaBase* Room)
{
    AllLoadedRooms.Remove(Room);

    if (OnRoomUnloaded.IsBound())
    {
        OnRoomUnloaded.Broadcast(Room);
    }
}

void URoomSystem::POIVisibility(bool bVisible)
{
    bPOIVisibility = bVisible;
}

void URoomSystem::POIDetectionVisibility(bool bVisible)
{
    bPOIDetectionVisibility = bVisible;
}

void URoomSystem::PlayerEnteredRoom(ARoomAreaBase* RoomEntered)
{
    if (!RoomEntered)
    {
        return;
    }

    PlayerCurrentRooms.AddUnique(RoomEntered);

    UWorld* World = GetWorld();
    float CurrentTime = World ? World->GetTimeSeconds() : 0.f;

    FPlayerRoomInfo* ExistingInfo = RoomPlayerExitTimes.Find(RoomEntered);
    if (ExistingInfo)
    {
        ExistingInfo->LastVisitTime = CurrentTime;
    }
    else
    {
        FPlayerRoomInfo NewInfo;
        NewInfo.LastVisitTime = CurrentTime;
        NewInfo.PlayerTotalTimeSpent = 0.f;
        NewInfo.PlayerTimeSpentRecent = 0.f;
        RoomPlayerExitTimes.Add(RoomEntered, NewInfo);
    }

    OnPlayerEnteredRoom.Broadcast(RoomEntered);
}

void URoomSystem::PlayerExitedRoom(ARoomAreaBase* RoomExited)
{
    if (!RoomExited)
    {
        return;
    }

    UWorld* World = GetWorld();
    float CurrentTime = World ? World->GetTimeSeconds() : 0.f;

    FPlayerRoomInfo* ExistingInfo = RoomPlayerExitTimes.Find(RoomExited);
    if (ExistingInfo)
    {
        ExistingInfo->LastVisitTime = CurrentTime;
    }
    else
    {
        FPlayerRoomInfo NewInfo;
        NewInfo.LastVisitTime = CurrentTime;
        NewInfo.PlayerTotalTimeSpent = 0.f;
        NewInfo.PlayerTimeSpentRecent = 0.f;
        RoomPlayerExitTimes.Add(RoomExited, NewInfo);
    }

    PlayerCurrentRooms.Remove(RoomExited);
    OnPlayerExitedRoom.Broadcast(RoomExited);
}

bool URoomSystem::IsPlayerInRoom(AActor* Room) const
{
    for (ARoomAreaBase* CurrentRoom : PlayerCurrentRooms)
    {
        if (CurrentRoom == Room)
        {
            return true;
        }
    }
    return false;
}

TArray<ARoomAreaBase*> URoomSystem::GetPlayerCurrentRooms() const
{
    return PlayerCurrentRooms;
}

void URoomSystem::GetPlayerRoomInfo(AActor* Room, FPlayerRoomInfo& RoomInfo, bool& Found) const
{
    const FPlayerRoomInfo* Info = RoomPlayerExitTimes.Find(Room);
    if (Info)
    {
        Found = true;
        RoomInfo = *Info;
    }
    else
    {
        Found = false;
        RoomInfo.LastVisitTime = 0.f;
        RoomInfo.PlayerTotalTimeSpent = 0.f;
        RoomInfo.PlayerTimeSpentRecent = 0.f;
    }
}

/* Returns 0 if player is currently in the room, FLT_MAX if the room has
   never been visited, otherwise TimeSeconds - LastVisitTime. */
float URoomSystem::GetPlayerVisitAgeForRoom(AActor* Room) const
{
    for (ARoomAreaBase* CurrentRoom : PlayerCurrentRooms)
    {
        if (CurrentRoom == Room)
        {
            return 0.f;
        }
    }

    UWorld* World = GetWorld();
    float CurrentTime = World ? World->GetTimeSeconds() : 0.f;

    const FPlayerRoomInfo* Info = RoomPlayerExitTimes.Find(Room);
    if (!Info)
    {
        return FLT_MAX;
    }

    return CurrentTime - Info->LastVisitTime;
}

float URoomSystem::GetPlayerTimeInRoom(AActor* Room) const
{
    const FPlayerRoomInfo* Info = RoomPlayerExitTimes.Find(Room);
    if (Info)
    {
        return Info->PlayerTotalTimeSpent;
    }
    return 0.f;
}

/* BFS that re-queues when a shorter path is found. Uses AdjacentRooms
   and resolves TSoftObjectPtr<ARoomAreaBase> per neighbor. */
TMap<ARoomAreaBase*, int32> URoomSystem::GetAllRoomDistancesFromRoom(ARoomAreaBase* Room) const
{
    TMap<ARoomAreaBase*, int32> Result;

    if (!Room)
    {
        return Result;
    }

    TArray<ARoomAreaBase*> RoomsToVisit;
    RoomsToVisit.Add(Room);
    Result.Add(Room, 0);

    while (RoomsToVisit.Num() > 0)
    {
        ARoomAreaBase* Current = RoomsToVisit.Pop();

        int32* CurrentDistPtr = Result.Find(Current);
        if (!CurrentDistPtr)
        {
            continue;
        }
        int32 NextDist = *CurrentDistPtr + 1;

        TArray<FRoomAdjacencyInfo> AdjacencyInfos = Current->GetAllAdjacentRooms();

        for (const FRoomAdjacencyInfo& AdjInfo : AdjacencyInfos)
        {
            ARoomAreaBase* Neighbor = AdjInfo.Room.Get();
            if (!Neighbor)
            {
                continue;
            }

            int32* ExistingDist = Result.Find(Neighbor);
            if (ExistingDist)
            {
                if (NextDist < *ExistingDist)
                {
                    RoomsToVisit.Add(Neighbor);
                    *ExistingDist = NextDist;
                }
            }
            else
            {
                RoomsToVisit.Add(Neighbor);
                Result.Add(Neighbor, NextDist);
            }
        }
    }

    return Result;
}

// Uses the LAST room in PlayerCurrentRooms, not all of them
TMap<ARoomAreaBase*, int32> URoomSystem::GetAllRoomDistancesFromPlayerRoom() const
{
    if (PlayerCurrentRooms.Num() <= 0)
    {
        return TMap<ARoomAreaBase*, int32>();
    }

    ARoomAreaBase* LastRoom = PlayerCurrentRooms.Last();
    return GetAllRoomDistancesFromRoom(LastRoom);
}

ARoomAreaBase* URoomSystem::GetRoomAtLocation(const FVector& Location) const
{
    for (ARoomAreaBase* Room : AllLoadedRooms)
    {
        if (Room && Room->IsLocationInRoom(Location))
        {
            return Room;
        }
    }
    return nullptr;
}

void URoomSystem::GetClosestPointOfInterest(const FVector& WorldLocation, bool& bOutValid, FPOIResult& OutResult) const
{
    OutResult.Index.Room = nullptr;
    OutResult.Index.Index = -1;

    float ClosestDistSq = FLT_MAX;

    for (ARoomAreaBase* Room : AllLoadedRooms)
    {
        if (!Room) continue;

        TArray<FPointOfInterestRuntimeInfo> POIInfos = Room->GetPointsOfInterestInfo();
        for (int32 i = 0; i < POIInfos.Num(); ++i)
        {
            float DistSq = FVector::DistSquared(WorldLocation, POIInfos[i].WorldLocation);
            if (DistSq < ClosestDistSq)
            {
                ClosestDistSq = DistSq;
                OutResult.Index.Room = Room;
                OutResult.Index.Index = i;
                OutResult.Info = POIInfos[i];
            }
        }
    }

    bOutValid = (OutResult.Index.Room != nullptr);
}

void URoomSystem::AdjustClosestPointOfInterestHeat(const FVector& WorldLocation, float Amount)
{
    ARoomAreaBase* ClosestRoom = nullptr;
    int32 ClosestIndex = -1;
    float ClosestDistSq = FLT_MAX;

    for (ARoomAreaBase* Room : AllLoadedRooms)
    {
        if (!Room) continue;

        TArray<FPointOfInterestRuntimeInfo> POIInfos = Room->GetPointsOfInterestInfo();
        for (int32 i = 0; i < POIInfos.Num(); ++i)
        {
            float DistSq = FVector::DistSquared(WorldLocation, POIInfos[i].WorldLocation);
            if (DistSq < ClosestDistSq)
            {
                ClosestDistSq = DistSq;
                ClosestRoom = Room;
                ClosestIndex = i;
            }
        }
    }

    if (ClosestRoom && ClosestIndex >= 0)
    {
        ClosestRoom->AdjustPOIHeat(ClosestIndex, Amount);
    }
}

void URoomSystem::GetPOIsInRange(const FVector& WorldLocation, float Radius, TArray<FPOIResult>& OutPointIndices) const
{
    float RadiusSq = Radius * Radius;

    for (ARoomAreaBase* Room : AllLoadedRooms)
    {
        if (!Room) continue;

        TArray<FPointOfInterestRuntimeInfo> POIInfos = Room->GetPointsOfInterestInfo();
        for (int32 i = 0; i < POIInfos.Num(); ++i)
        {
            float DistSq = FVector::DistSquared(WorldLocation, POIInfos[i].WorldLocation);
            if (DistSq < RadiusSq)
            {
                FPOIResult Result;
                Result.Index.Room = Room;
                Result.Index.Index = i;
                Result.Info = POIInfos[i];
                OutPointIndices.Add(Result);
            }
        }
    }
}

/* The original uses a latent action with GatherNavLengths async worker for
   true navigable distance. Simplified here to straight-line distance since we
   don't have FPointsInNavigableRangeLatentAction/GatherNavLengths classes. */
void URoomSystem::GetPOIsInNavigableRange(APawn* NavigationPawn, float Radius, TArray<FPOIResult>& OutPOIs, FLatentActionInfo LatentActionInfo) const
{
    if (!NavigationPawn)
    {
        return;
    }

    FVector Location = FVector::ZeroVector;
    if (NavigationPawn->GetRootComponent())
    {
        Location = NavigationPawn->GetRootComponent()->GetComponentLocation();
    }

    TArray<FPOIResult> CandidatePOIs;
    GetPOIsInRange(Location, Radius, CandidatePOIs);

    // TODO: Implement FPointsInNavigableRangeLatentAction with GatherNavLengths
    OutPOIs = CandidatePOIs;
}

void URoomSystem::GetHighestHeatPOIFromArray(const TArray<FPOIResult>& POIArray, bool& bValid, FPOIResult& OutResult) const
{
    bValid = false;
    float HighestHeat = 0.f;

    OutResult.Index.Room = nullptr;
    OutResult.Index.Index = -1;

    for (const FPOIResult& Result : POIArray)
    {
        if (Result.Info.CurrentHeat > HighestHeat)
        {
            bValid = true;
            HighestHeat = Result.Info.CurrentHeat;
            OutResult = Result;
        }
    }
}

// Weight per POI is CurrentHeat + 0.001 (not + 1.0)
void URoomSystem::GetWeightedRandomPOIFromArray(const TArray<FPOIResult>& POIArray, bool& bOutValid, FPOIResult& OutResult) const
{
    bOutValid = false;

    if (POIArray.Num() == 0)
    {
        return;
    }

    float TotalWeight = 0.f;
    for (const FPOIResult& Result : POIArray)
    {
        TotalWeight += (Result.Info.CurrentHeat + 0.001f);
    }

    float RandomVal = FMath::FRand() * TotalWeight;

    float Accumulated = 0.f;
    for (const FPOIResult& Result : POIArray)
    {
        Accumulated += (Result.Info.CurrentHeat + 0.001f);
        if (RandomVal <= Accumulated)
        {
            bOutValid = true;
            OutResult = Result;
            return;
        }
    }
}

TArray<FPOIResult> URoomSystem::RemovePOIsFromArray(TArray<FPOIIndex>& POIsToRemove, TArray<FPOIResult>& POIArray) const
{
    int32 WriteIndex = 0;

    for (int32 i = 0; i < POIArray.Num(); /* manually increment */)
    {
        bool bShouldRemove = false;

        for (const FPOIIndex& RemoveIdx : POIsToRemove)
        {
            if (POIArray[i].Index.Room == RemoveIdx.Room && POIArray[i].Index.Index == RemoveIdx.Index)
            {
                bShouldRemove = true;
                break;
            }
        }

        if (bShouldRemove)
        {
            if (POIArray.Num() - i > 1)
            {
                FMemory::Memmove(&POIArray[i], &POIArray[i + 1], sizeof(FPOIResult) * (POIArray.Num() - i - 1));
            }
            POIArray.SetNum(POIArray.Num() - 1, false);
        }
        else
        {
            ++i;
        }
    }

    return POIArray;
}

void URoomSystem::RoomSystemTick()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    float DeltaTime = World->GetDeltaSeconds();

    // Accumulate time for rooms the player is currently in
    for (ARoomAreaBase* PlayerRoom : PlayerCurrentRooms)
    {
        FPlayerRoomInfo* Info = RoomPlayerExitTimes.Find(PlayerRoom);
        if (Info)
        {
            Info->PlayerTotalTimeSpent += RoomUpdateTick;
            Info->PlayerTimeSpentRecent += RoomUpdateTick;
        }
    }

    // Decay PlayerTimeSpentRecent for rooms the player is NOT in
    const URoomSystemSettings* Settings = GetDefault<URoomSystemSettings>();
    float RecentDecayRate = Settings ? Settings->PlayerRoomRecentDecay : 0.1f;

    for (auto& Pair : RoomPlayerExitTimes)
    {
        bool bPlayerInRoom = PlayerCurrentRooms.Contains(Cast<ARoomAreaBase>(Pair.Key));

        if (!bPlayerInRoom)
        {
            Pair.Value.PlayerTimeSpentRecent -= RecentDecayRate * DeltaTime;
        }
    }

    // Increase heat on the closest POI to the player
    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!PlayerPawn)
    {
        return;
    }

    FVector PlayerLocation = FVector::ZeroVector;
    if (PlayerPawn->GetRootComponent())
    {
        PlayerLocation = PlayerPawn->GetRootComponent()->GetComponentLocation();
    }

    ARoomAreaBase* ClosestRoom = nullptr;
    int32 ClosestIndex = -1;
    float ClosestDistSq = FLT_MAX;

    for (ARoomAreaBase* Room : AllLoadedRooms)
    {
        if (!Room) continue;

        TArray<FPointOfInterestRuntimeInfo> POIInfos = Room->GetPointsOfInterestInfo();
        for (int32 i = 0; i < POIInfos.Num(); ++i)
        {
            float DistSq = FVector::DistSquared(PlayerLocation, POIInfos[i].WorldLocation);
            if (DistSq < ClosestDistSq)
            {
                ClosestDistSq = DistSq;
                ClosestRoom = Room;
                ClosestIndex = i;
            }
        }
    }

    if (ClosestRoom && ClosestIndex >= 0)
    {
        float HeatIncrease = Settings ? Settings->POINearIncreasePerSecond * RoomUpdateTick : 0.f;
        ClosestRoom->AdjustPOIHeat(ClosestIndex, HeatIncrease);
    }
}