#include "RoomAreaBase.h"
#include "RoomSystem.h"
#include "RoomSystemSettings.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Kismet/GameplayStatics.h"

static const FName HeatAITag(TEXT("HeatAI"));
static const FName PlayerTag(TEXT("Player"));

ARoomAreaBase::ARoomAreaBase()
{
    MapName = NAME_None;
    bPlayerIsInRoom = false;
    PlayerHeatValue = 0.f;
    AIHeatValue = 0.f;

    PrimaryActorTick.bCanEverTick = true;

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    CharacterDetectorsRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CharacterDetectors"));
    CharacterDetectorsRoot->SetupAttachment(RootComponent);
}

void ARoomAreaBase::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();

    TArray<USceneComponent*> Children;
    CharacterDetectorsRoot->GetChildrenComponents(true, Children);

    CharacterDetectors.Empty();
    for (USceneComponent* Child : Children)
    {
        UBoxComponent* BoxComp = Cast<UBoxComponent>(Child);
        if (BoxComp)
        {
            CharacterDetectors.Add(BoxComp);
        }
    }
}

void ARoomAreaBase::BeginPlay()
{
    Super::BeginPlay();

    // Pick up actors already overlapping at start
    TArray<AActor*> OverlappingActors;
    GetOverlappingActors(OverlappingActors);

    for (AActor* Actor : OverlappingActors)
    {
        if (!Actor) continue;

        if (Actor->ActorHasTag(HeatAITag))
        {
            AICharactersInRoom.AddUnique(Actor);
        }
        else if (Actor->ActorHasTag(PlayerTag))
        {
            bPlayerIsInRoom = true;
            UWorld* World = GetWorld();
            if (World)
            {
                URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
                if (RoomSys)
                {
                    RoomSys->PlayerEnteredRoom(this);
                }
            }
        }
    }

    // Initialize runtime POI infos from editor-placed PointsOfInterest
    PointOfInterestRuntimeInfos.Empty();
    PointOfInterestRuntimeInfos.Reserve(PointsOfInterest.Num());
    for (const FVector& POI : PointsOfInterest)
    {
        FPointOfInterestRuntimeInfo Info;
        Info.CurrentHeat = 0.f;
        Info.VisitTime = 0.f;
        Info.WorldLocation = POI;
        PointOfInterestRuntimeInfos.Add(Info);
    }

    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys)
        {
            RoomSys->RegisterRoom(this);
        }
    }
}

void ARoomAreaBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys)
        {
            RoomSys->UnregisterRoom(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}

void ARoomAreaBase::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    const URoomSystemSettings* Settings = GetDefault<URoomSystemSettings>();
    if (!Settings)
    {
        return;
    }

    if (bPlayerIsInRoom)
    {
        PlayerHeatValue += DeltaTime * Settings->PlayerRoomHeatIncreaseRate;
    }
    else
    {
        PlayerHeatValue -= DeltaTime * Settings->PlayerRoomHeatDecayRate;
    }
    PlayerHeatValue = FMath::Clamp(PlayerHeatValue, 0.f, 1.f);

    int32 NumAIInRoom = AICharactersInRoom.Num();
    if (NumAIInRoom > 0)
    {
        AIHeatValue += DeltaTime * Settings->AIRoomHeatIncreaseRate * (float)NumAIInRoom;
    }
    else
    {
        AIHeatValue -= DeltaTime * Settings->AIRoomHeatDecayRate;
    }
    AIHeatValue = FMath::Clamp(AIHeatValue, 0.f, 1.f);

    // POI debug display when bPOIVisibility is enabled
    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys && RoomSys->IsPOIVisible())
        {
            for (const FPointOfInterestRuntimeInfo& POIInfo : PointOfInterestRuntimeInfos)
            {
                // Color based on heat: green (cold) → red (hot)
                float HeatNormalized = FMath::Clamp(POIInfo.CurrentHeat / FMath::Max(GetDefault<URoomSystemSettings>()->MaxPOIHeat, 1.f), 0.f, 1.f);
                FColor DebugColor = FColor::MakeRedToGreenColorFromScalar(1.f - HeatNormalized);

                // Draw ? marker at POI world location
                FString DebugText = FString::Printf(TEXT("? %.1f"), POIInfo.CurrentHeat);
                DrawDebugString(World, POIInfo.WorldLocation, DebugText, nullptr, DebugColor, 0.f, true, 1.2f);
                DrawDebugPoint(World, POIInfo.WorldLocation, 8.f, DebugColor, false, 0.f);
            }

            // Also draw room heat info
            if (CharacterDetectors.Num() > 0 && CharacterDetectors[0])
            {
                FVector RoomCenter = CharacterDetectors[0]->GetComponentLocation();
                FString RoomDebugText = FString::Printf(TEXT("Room: P=%.2f AI=%.2f %s"),
                    PlayerHeatValue, AIHeatValue,
                    bPlayerIsInRoom ? TEXT("[PLAYER]") : TEXT(""));
                DrawDebugString(World, RoomCenter + FVector(0, 0, 100), RoomDebugText, nullptr, FColor::Cyan, 0.f, true, 1.5f);
            }
        }
    }
}

void ARoomAreaBase::NotifyActorBeginOverlap(AActor* OtherActor)
{
    Super::NotifyActorBeginOverlap(OtherActor);
    AddActorToRoom(OtherActor);
}

void ARoomAreaBase::NotifyActorEndOverlap(AActor* OtherActor)
{
    Super::NotifyActorEndOverlap(OtherActor);

    if (OtherActor->ActorHasTag(HeatAITag))
    {
        AICharactersInRoom.Remove(OtherActor);
    }
    else if (OtherActor->ActorHasTag(PlayerTag))
    {
        bPlayerIsInRoom = false;

        UWorld* World = GetWorld();
        if (World)
        {
            URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
            if (RoomSys)
            {
                RoomSys->PlayerExitedRoom(this);
            }
        }
    }
}

void ARoomAreaBase::AddActorToRoom(AActor* OtherActor)
{
    if (!OtherActor) return;

    if (OtherActor->ActorHasTag(HeatAITag))
    {
        AICharactersInRoom.AddUnique(OtherActor);
    }
    else if (OtherActor->ActorHasTag(PlayerTag))
    {
        bPlayerIsInRoom = true;

        UWorld* World = GetWorld();
        if (World)
        {
            URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
            if (RoomSys)
            {
                RoomSys->PlayerEnteredRoom(this);
            }
        }
    }
}

void ARoomAreaBase::SetPOIVisited(int32 Index)
{
    if (Index >= 0 && Index < PointOfInterestRuntimeInfos.Num())
    {
        UWorld* World = GetWorld();
        PointOfInterestRuntimeInfos[Index].VisitTime = World ? World->GetTimeSeconds() : 0.f;
    }
}

// VisitTime is set to -1.0f (not 0) so that HasPOIBeenVisited (> -1.0) works correctly
void ARoomAreaBase::ClearPOIVisited(int32 Index)
{
    if (Index >= 0 && Index < PointOfInterestRuntimeInfos.Num())
    {
        PointOfInterestRuntimeInfos[Index].VisitTime = -1.f;
    }
}

void ARoomAreaBase::ResetPOIHeat(int32 Index)
{
    if (Index >= 0 && Index < PointOfInterestRuntimeInfos.Num())
    {
        PointOfInterestRuntimeInfos[Index].CurrentHeat = 0.f;
    }
}

void ARoomAreaBase::AdjustPOIHeat(int32 Index, float Amount)
{
    if (Index < 0 || Index >= PointOfInterestRuntimeInfos.Num())
    {
        return;
    }

    const URoomSystemSettings* Settings = GetDefault<URoomSystemSettings>();
    float MaxHeat = Settings ? Settings->MaxPOIHeat : 10.f;

    float NewHeat = PointOfInterestRuntimeInfos[Index].CurrentHeat + Amount;

    if (NewHeat < 0.f)
    {
        NewHeat = 0.f;
    }
    else
    {
        NewHeat = FMath::Min(NewHeat, MaxHeat);
    }

    PointOfInterestRuntimeInfos[Index].CurrentHeat = NewHeat;
}

void ARoomAreaBase::AdjustPOIHeatCollect(int32 Index)
{
    const URoomSystemSettings* Settings = GetDefault<URoomSystemSettings>();
    float CollectAmount = Settings ? Settings->POIHeatIncreaseOnCollect : 1.f;

    AdjustPOIHeat(Index, CollectAmount);
}

bool ARoomAreaBase::IsLocationInRoom(const FVector& Location) const
{
    for (UBoxComponent* Detector : CharacterDetectors)
    {
        if (!Detector) continue;

        FVector LocalPoint = Detector->GetComponentTransform().InverseTransformPosition(Location);
        FVector Extent = Detector->GetUnscaledBoxExtent();

        if (FMath::Abs(LocalPoint.X) < Extent.X &&
            FMath::Abs(LocalPoint.Y) < Extent.Y &&
            FMath::Abs(LocalPoint.Z) < Extent.Z)
        {
            return true;
        }
    }
    return false;
}

float ARoomAreaBase::GetTotalRoomArea() const
{
    float AreaX = 0.f;
    float AreaY = 0.f;

    for (UBoxComponent* Detector : CharacterDetectors)
    {
        if (!Detector) continue;

        FVector Scale = Detector->GetComponentScale();
        FVector Extent = Detector->GetUnscaledBoxExtent();

        /* IDA accumulates (Scale * 2) * Extent per-axis separately, but the offset
           mapping is ambiguous. Fall through to the scaled-extent path below. */
        AreaX += (Scale.X * 2.f) * Extent.Z;
        AreaY += (Scale.Y * 2.f) * Extent.Y;
    }

    float TotalArea = 0.f;
    for (UBoxComponent* Detector : CharacterDetectors)
    {
        if (!Detector) continue;
        FVector ScaledExtent = Detector->GetScaledBoxExtent();
        TotalArea += (ScaledExtent.X * 2.f) * (ScaledExtent.Y * 2.f);
    }
    return TotalArea;
}

// Generates a grid of points inside each detector box using forward/right vectors
TArray<FVector> ARoomAreaBase::GetRoomPoints(float PointDelta)
{
    TArray<FVector> Points;

    if (PointDelta <= 0.f)
    {
        return Points;
    }

    for (UBoxComponent* Detector : CharacterDetectors)
    {
        if (!Detector) continue;

        FVector Origin = Detector->GetComponentLocation();
        FVector ForwardVec = Detector->GetForwardVector();
        FVector RightVec = Detector->GetRightVector();
        FVector Scale = Detector->GetComponentScale();
        FVector Extent = Detector->GetUnscaledBoxExtent();

        float ForwardExtent = Scale.X * Extent.X;
        float RightExtent = Scale.Y * Extent.Y;

        for (float Y = -RightExtent; Y <= RightExtent; Y += PointDelta)
        {
            for (float X = -ForwardExtent; X <= ForwardExtent; X += PointDelta)
            {
                FVector Point = Origin + ForwardVec * X + RightVec * Y;
                Point.Z = Detector->GetComponentLocation().Z;
                Points.Add(Point);
            }
        }
    }

    return Points;
}

FVector ARoomAreaBase::GetRandomLocationInRoom() const
{
    if (CharacterDetectors.Num() <= 0)
    {
        return GetActorLocation();
    }

    int32 Idx = FMath::RandRange(0, CharacterDetectors.Num() - 1);
    UBoxComponent* Detector = CharacterDetectors[Idx];
    if (!Detector)
    {
        return GetActorLocation();
    }

    FVector Extent = Detector->GetUnscaledBoxExtent();
    FBox Box(FVector(-Extent.X, -Extent.Y, -Extent.Z), Extent);
    FVector LocalPoint = FMath::RandPointInBox(Box);

    return Detector->GetComponentTransform().TransformPosition(LocalPoint);
}

FVector ARoomAreaBase::GetRoomEntryPoint(int32 EntryIndex) const
{
    const FVector* Found = RoomEntryPoints.Find(EntryIndex);
    if (!Found)
    {
        return FVector::ZeroVector;
    }

    // Entry points are stored in local space; transform to world
    if (RootComponent)
    {
        return RootComponent->GetComponentTransform().TransformPosition(*Found);
    }

    return *Found;
}

TMap<int32, FVector> ARoomAreaBase::GetRoomEntryPoints() const
{
    return RoomEntryPoints;
}

TArray<FRoomAdjacencyInfo> ARoomAreaBase::GetRoomAdjacency(const ARoomAreaBase* Room) const
{
    TArray<FRoomAdjacencyInfo> Result;
    for (const FRoomAdjacencyInfo& Info : AdjacentRooms)
    {
        if (Info.Room.Get() == Room)
        {
            Result.Add(Info);
        }
    }
    return Result;
}

TArray<FRoomAdjacencyInfo> ARoomAreaBase::GetAllAdjacentRooms() const
{
    return AdjacentRooms;
}

TArray<FRoomAdjacencyInfo> ARoomAreaBase::GetAllAdjacentRoomInfos() const
{
    return AdjacentRooms;
}

FRoomAdjacencyInfo ARoomAreaBase::GetAdjacentInfoFromDoor(AActor* Door)
{
    for (const FRoomAdjacencyInfo& Info : AdjacentRooms)
    {
        if (Info.DoorActor.Get() == Door)
        {
            return Info;
        }
    }

    return FRoomAdjacencyInfo();
}

TArray<FPointOfInterestRuntimeInfo> ARoomAreaBase::GetPointsOfInterestInfo() const
{
    return PointOfInterestRuntimeInfos;
}

TArray<FVector> ARoomAreaBase::GetPointsOfInterest() const
{
    return PointsOfInterest;
}

FPointOfInterestRuntimeInfo ARoomAreaBase::GetPointOfInterestInfoByIndex(int32 Index) const
{
    if (Index >= 0 && Index < PointOfInterestRuntimeInfos.Num())
    {
        return PointOfInterestRuntimeInfos[Index];
    }

    static FPointOfInterestRuntimeInfo GInvalidPointInfo;
    return GInvalidPointInfo;
}

// These return room-level heat values, not the sum of individual POI heats
float ARoomAreaBase::GetPlayerHeat() const
{
    return PlayerHeatValue;
}

float ARoomAreaBase::GetAIHeat() const
{
    return AIHeatValue;
}

FName ARoomAreaBase::GetMapName() const
{
    return MapName;
}

TArray<AActor*> ARoomAreaBase::GetDoors() const
{
    TArray<AActor*> Doors;
    for (const FRoomAdjacencyInfo& Info : AdjacentRooms)
    {
        AActor* Door = Info.DoorActor.Get();
        if (Door)
        {
            Doors.Add(Door);
        }
    }
    return Doors;
}

TArray<UBoxComponent*> ARoomAreaBase::GetDetectors() const
{
    return CharacterDetectors;
}

USceneComponent* ARoomAreaBase::GetCharacterDetectorRoot() const
{
    return CharacterDetectorsRoot;
}

TArray<AActor*> ARoomAreaBase::GetAllHideActors() const
{
    return HideActors;
}

TArray<AActor*> ARoomAreaBase::GetAllAIHideActors() const
{
    return AIHideActors;
}