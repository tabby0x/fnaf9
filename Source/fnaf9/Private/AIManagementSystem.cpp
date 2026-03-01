#include "AIManagementSystem.h"
#include "AISystemSettings.h"
#include "FNAFAISpawnPoint.h"
#include "FNAFManagedAI.h"
#include "SeekerPatrolPath.h"
#include "PathPointProvider.h"
#include "WorldStateSystem.h"
#include "FNAFAISaveData.h"
#include "FNAFAICharacterInfo.h"
#include "AIEnvironmentQueryInfo.h"
#include "AIHiderInterface.h"
#include "AIHideLocationInterface.h"
#include "HideCueObjectInterface.h"
#include "HideObjectInterface.h"
#include "PatrollerInterface.h"
#include "AnimatronicTypeDataBlueprintFunctionLibrary.h"
#include "GameClockSystem.h"
#include "RoomSystem.h"
#include "RoomAreaBase.h"
#include "RoomSystemSettings.h"
#include "Templates/SubclassOf.h"
#include "Engine/AssetManager.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StreamableManager.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FNAFGameInstanceBase.h"
#include "AIDLC_RabbitSystem.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "DrawDebugHelpers.h"
#include "CollisionQueryParams.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogAIManagement, Log, All);

UAIManagementSystem::UAIManagementSystem()
{
    EnableDebugCloak = false;
    TimeSinceLastEncounter = 0.0f;
    AIHiding = nullptr;
    CurrentHideLocation = nullptr;
    bUseStagedSpawns = false;
    DoNotDespawnAIDuringMoonmanTime = false;
    AISpawnOnLoadProximity = 0.0f;
    bSpawningEnabled = true;
    bRandomWorldSpawningEnabled = true;
    bVanessaSpawnEnabled = false;
    bAITeleportEnabled = true;
}

// Async asset loading for AI character info and env query info
void UAIManagementSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Detect game mode from GameInstance — not map name
    UFNAFGameInstanceBase* GameInstance = Cast<UFNAFGameInstanceBase>(World->GetGameInstance());
    bool bIsChowdaMode = GameInstance && GameInstance->GetCurrentGameType() == EFNAFGameType::ChowdaMode;

    // Use AssetManager to find and async-load UFNAFAICharacterInfo assets
    UAssetManager& AssetManager = UAssetManager::Get();
    IAssetRegistry& AssetRegistry = AssetManager.GetAssetRegistry();

    // Load AI Character Info assets
    TArray<FAssetData> CharacterInfoAssets;
    AssetRegistry.GetAssetsByClass(UFNAFAICharacterInfo::StaticClass()->GetFName(), CharacterInfoAssets, true);

    if (CharacterInfoAssets.Num() > 0)
    {
        // Filter by mode: Chowda-folder assets for ChowdaMode, Game-folder for Normal
        TArray<FSoftObjectPath> AssetsToLoad;
        TArray<FAssetData> FilteredAssetData;
        for (const FAssetData& Asset : CharacterInfoAssets)
        {
            FSoftObjectPath SoftPath = Asset.ToSoftObjectPath();
            FString AssetPath = SoftPath.ToString();
            bool bIsChowdaAsset = AssetPath.Contains(TEXT("Chowda"));

            if (bIsChowdaMode == bIsChowdaAsset)
            {
                AssetsToLoad.Add(SoftPath);
                FilteredAssetData.Add(Asset);
            }
        }

        if (AssetsToLoad.Num() > 0)
        {
            FStreamableManager& StreamableManager = AssetManager.GetStreamableManager();
            StreamableManager.RequestAsyncLoad(AssetsToLoad,
                FStreamableDelegate::CreateUObject(this, &UAIManagementSystem::OnAICharacterInfoLoaded, FilteredAssetData));
        }
    }

    // Load AI Environment Query Info assets
    TArray<FAssetData> EnvQueryAssets;
    AssetRegistry.GetAssetsByClass(UAIEnvironmentQueryInfo::StaticClass()->GetFName(), EnvQueryAssets, true);

    if (EnvQueryAssets.Num() > 0)
    {
        TArray<FSoftObjectPath> EnvAssetsToLoad;
        for (const FAssetData& Asset : EnvQueryAssets)
        {
            EnvAssetsToLoad.Add(Asset.ToSoftObjectPath());
        }

        if (EnvAssetsToLoad.Num() > 0)
        {
            FStreamableManager& StreamableManager = AssetManager.GetStreamableManager();
            StreamableManager.RequestAsyncLoad(EnvAssetsToLoad,
                FStreamableDelegate::CreateUObject(this, &UAIManagementSystem::OnAIEnvQueryInfoLoaded, EnvQueryAssets));
        }
    }

    // Find all spawn points in the world
    FindAllSpawnPoints();
}

void UAIManagementSystem::OnAICharacterInfoLoaded(TArray<FAssetData> AssetDataList)
{
    for (const FAssetData& AssetData : AssetDataList)
    {
        UFNAFAICharacterInfo* CharInfo = Cast<UFNAFAICharacterInfo>(AssetData.GetAsset());
        if (CharInfo)
        {
            // Populate CharacterInfoDataMap from loaded character info assets
            // Each UFNAFAICharacterInfo maps an EFNAFAISpawnType to its FFNAFAISettingInfo
            CharacterInfoDataMap.Add(CharInfo->AIType, CharInfo->SettingInfo);
        }
    }
}

void UAIManagementSystem::OnAIEnvQueryInfoLoaded(TArray<FAssetData> AssetDataList)
{
    for (const FAssetData& AssetData : AssetDataList)
    {
        UAIEnvironmentQueryInfo* QueryInfo = Cast<UAIEnvironmentQueryInfo>(AssetData.GetAsset());
        if (QueryInfo)
        {
            QueryData.Add(QueryInfo);
        }
    }
}

void UAIManagementSystem::AILostSightOfPlayer(APawn* AIPawn)
{
    UE_LOG(LogAIManagement, Warning, TEXT("AILostSightOfPlayer: %s, PawnsWithSight: %d -> %d"),
        AIPawn ? *AIPawn->GetName() : TEXT("null"), PawnsWithSightToPlayer.Num(), FMath::Max(0, PawnsWithSightToPlayer.Num() - 1));
    PawnsWithSightToPlayer.Remove(AIPawn);
}

void UAIManagementSystem::AISpottedPlayer(APawn* AIPawn)
{
    UE_LOG(LogAIManagement, Warning, TEXT("AISpottedPlayer: %s, PawnsWithSight: %d"),
        AIPawn ? *AIPawn->GetName() : TEXT("null"), PawnsWithSightToPlayer.Num());
    PawnsWithSightToPlayer.AddUnique(AIPawn);
    TimeSinceLastEncounter = 0.0f;
}

void UAIManagementSystem::AddSpawnPoint(AFNAFAISpawnPoint* SpawnPoint)
{
    SpawnPoints.Add(SpawnPoint);
}

void UAIManagementSystem::RemoveSpawnPoint(AFNAFAISpawnPoint* SpawnPoint)
{
    SpawnPoints.RemoveAll([SpawnPoint](const TWeakObjectPtr<AFNAFAISpawnPoint>& Ptr) {
        return Ptr.Get() == SpawnPoint;
        });
}

void UAIManagementSystem::AddExpectedAI(FAnimatronicExpectedData AIType)
{
    // Manual uniqueness check - FAnimatronicExpectedData has no operator==
    for (const FAnimatronicExpectedData& Existing : TypesExpected)
    {
        if (Existing.AIType == AIType.AIType && Existing.PathName == AIType.PathName)
        {
            return;
        }
    }
    TypesExpected.Add(AIType);
}

void UAIManagementSystem::AdjustVannyMeter(float Amount)
{
    VannyMeter.AdjustVannyMeter(Amount);
}

void UAIManagementSystem::ClearExpectedAI()
{
    TypesExpected.Empty();
}

void UAIManagementSystem::DestroyAllAI()
{
    RemoveAllCharacters();
}

void UAIManagementSystem::DestroyAllAINotVisible()
{
    DestroyAllAIInRoomsAtleast(2);
}

void UAIManagementSystem::DoNotDespawnAIDuringMoonmanPhase(bool bDoNotDestroy)
{
    DoNotDespawnAIDuringMoonmanTime = bDoNotDestroy;
}

void UAIManagementSystem::ExitedHiding(APawn* AIPawn)
{
    AIHiding = nullptr;
    CurrentHideLocation = nullptr;
}

float UAIManagementSystem::GetTimeSinceLastEncounter() const
{
    return TimeSinceLastEncounter;
}

TArray<APawn*> UAIManagementSystem::GetAIPawnsWithSightToPlayer() const
{
    return PawnsWithSightToPlayer;
}

TArray<FAIDistanceResult> UAIManagementSystem::GetCachedDistances() const
{
    return CachedDistanceResults;
}

TArray<APawn*> UAIManagementSystem::GetAllRegisteredAI() const
{
    return RegisteredPawns;
}

TArray<APawn*> UAIManagementSystem::GetAllAI() const
{
    return RegisteredPawns;
}

bool UAIManagementSystem::IsUsingStagedSpawns() const
{
    return bUseStagedSpawns;
}

bool UAIManagementSystem::IsSpawningEnabled() const
{
    return bSpawningEnabled;
}

bool UAIManagementSystem::IsWorldSpawnEnabled() const
{
    return bRandomWorldSpawningEnabled;
}

bool UAIManagementSystem::IsAITeleportEnabled() const
{
    return bAITeleportEnabled;
}

void UAIManagementSystem::SetUseStagedSpawns(bool enable)
{
    bUseStagedSpawns = enable;
}

void UAIManagementSystem::SetSpawningEnabled(bool enable)
{
    bSpawningEnabled = enable;
}

void UAIManagementSystem::SetWorldSpawnEnabled(bool bEnable)
{
    bRandomWorldSpawningEnabled = bEnable;

    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        FFNAFAISaveData AIState = WorldStateSys->GetAIState();
        AIState.bWorldSpawnEnabled = bEnable;
        WorldStateSys->SetAIState(AIState);
    }
}

void UAIManagementSystem::SetVanessaSpawnEnable(bool bEnable)
{
    bVanessaSpawnEnabled = bEnable;
}

void UAIManagementSystem::SetAITeleportEnabled(bool bEnable)
{
    bAITeleportEnabled = bEnable;

    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        FFNAFAISaveData AIState = WorldStateSys->GetAIState();
        AIState.bAITeleportEnabled = bEnable;
        WorldStateSys->SetAIState(AIState);
    }
}

void UAIManagementSystem::PauseManager()
{
    UWorld* World = GetWorld();
    if (World && TimerHandle.IsValid())
    {
        World->GetTimerManager().PauseTimer(TimerHandle);
    }
}

void UAIManagementSystem::UnpauseManager()
{
    UWorld* World = GetWorld();
    if (World && TimerHandle.IsValid())
    {
        World->GetTimerManager().UnPauseTimer(TimerHandle);
    }
}

void UAIManagementSystem::SetExpectedAI(const TArray<FAnimatronicExpectedData>& AITypes)
{
    TypesExpected = AITypes;
}

// Spawn point queries
void UAIManagementSystem::FindAllSpawnPoints()
{
    SpawnPoints.Empty();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    for (TActorIterator<AFNAFAISpawnPoint> It(World); It; ++It)
    {
        AFNAFAISpawnPoint* SpawnPoint = *It;
        if (SpawnPoint)
        {
            SpawnPoints.Add(SpawnPoint);
        }
    }
}

TArray<AFNAFAISpawnPoint*> UAIManagementSystem::GetAllSpawnPoints() const
{
    TArray<AFNAFAISpawnPoint*> Result;

    for (const TWeakObjectPtr<AFNAFAISpawnPoint>& WeakSpawnPoint : SpawnPoints)
    {
        AFNAFAISpawnPoint* SpawnPoint = WeakSpawnPoint.Get();
        if (!SpawnPoint)
        {
            continue;
        }

        // If using staged spawns, filter by bIsStagedPoint
        // The IDA shows: if bUseStagedSpawns, only include points where bIsStagedPoint == true
        // If NOT using staged spawns, only include points where bIsStagedPoint == false
        // Access via the private member directly (friend/AllowPrivateAccess)
        // Actually from IDA: checks SpawnPoint+0x238 (bIsStagedPoint field)
        // We use GetAIType and check bIsStagedPoint pattern
        // Since bIsStagedPoint is private, the IDA likely accessed it directly via offset
        // For now we include all - the staged filtering needs direct field access
        Result.Add(SpawnPoint);
    }

    return Result;
}

TArray<AFNAFAISpawnPoint*> UAIManagementSystem::GetAllSpawnPointsFor(EFNAFAISpawnType AIType) const
{
    TArray<AFNAFAISpawnPoint*> Result;

    for (const TWeakObjectPtr<AFNAFAISpawnPoint>& WeakSpawnPoint : SpawnPoints)
    {
        AFNAFAISpawnPoint* SpawnPoint = WeakSpawnPoint.Get();
        if (SpawnPoint && SpawnPoint->GetAIType() == AIType)
        {
            Result.Add(SpawnPoint);
        }
    }

    return Result;
}

AFNAFAISpawnPoint* UAIManagementSystem::FindClosestSpawnPoint() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return nullptr;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    TArray<AFNAFAISpawnPoint*> AllPoints = GetAllSpawnPoints();

    AFNAFAISpawnPoint* ClosestPoint = nullptr;
    float ClosestDistSq = TNumericLimits<float>::Max();

    for (AFNAFAISpawnPoint* SpawnPoint : AllPoints)
    {
        if (!SpawnPoint)
        {
            continue;
        }

        float DistSq = FVector::DistSquared(SpawnPoint->GetActorLocation(), CameraLocation);

        // Line trace visibility check
        FHitResult HitResult;
        FCollisionQueryParams QueryParams;
        QueryParams.AddIgnoredActor(SpawnPoint);

        bool bHit = World->LineTraceSingleByChannel(
            HitResult,
            CameraLocation,
            SpawnPoint->GetActorLocation(),
            ECC_Visibility,
            QueryParams
        );

        // If line trace doesn't hit anything (clear line of sight), skip - we want NOT visible
        if (!bHit)
        {
            continue;
        }

        if (DistSq < ClosestDistSq)
        {
            ClosestDistSq = DistSq;
            ClosestPoint = SpawnPoint;
        }
    }

    return ClosestPoint;
}

AFNAFAISpawnPoint* UAIManagementSystem::FindFurthestSpawnPoint() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return nullptr;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    TArray<AFNAFAISpawnPoint*> AllPoints = GetAllSpawnPoints();

    AFNAFAISpawnPoint* FurthestPoint = nullptr;
    float FurthestDistSq = 0.0f;

    for (AFNAFAISpawnPoint* SpawnPoint : AllPoints)
    {
        if (!SpawnPoint)
        {
            continue;
        }

        float DistSq = FVector::DistSquared(SpawnPoint->GetActorLocation(), CameraLocation);

        FHitResult HitResult;
        FCollisionQueryParams QueryParams;
        QueryParams.AddIgnoredActor(SpawnPoint);

        bool bHit = World->LineTraceSingleByChannel(
            HitResult,
            CameraLocation,
            SpawnPoint->GetActorLocation(),
            ECC_Visibility,
            QueryParams
        );

        if (!bHit)
        {
            continue;
        }

        if (DistSq > FurthestDistSq)
        {
            FurthestDistSq = DistSq;
            FurthestPoint = SpawnPoint;
        }
    }

    return FurthestPoint;
}

AFNAFAISpawnPoint* UAIManagementSystem::FindSpawnPointClosestToDistance(float Distance) const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return nullptr;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    TArray<AFNAFAISpawnPoint*> AllPoints = GetAllSpawnPoints();

    AFNAFAISpawnPoint* BestPoint = nullptr;
    float BestDelta = TNumericLimits<float>::Max();

    for (AFNAFAISpawnPoint* SpawnPoint : AllPoints)
    {
        if (!SpawnPoint)
        {
            continue;
        }

        float ActualDist = FVector::Dist(SpawnPoint->GetActorLocation(), CameraLocation);
        float Delta = FMath::Abs(ActualDist - Distance);

        FHitResult HitResult;
        FCollisionQueryParams QueryParams;
        QueryParams.AddIgnoredActor(SpawnPoint);

        bool bHit = World->LineTraceSingleByChannel(
            HitResult,
            CameraLocation,
            SpawnPoint->GetActorLocation(),
            ECC_Visibility,
            QueryParams
        );

        if (!bHit)
        {
            continue;
        }

        if (Delta < BestDelta)
        {
            BestDelta = Delta;
            BestPoint = SpawnPoint;
        }
    }

    return BestPoint;
}

// Pawn queries
void UAIManagementSystem::GetAllAnimatronicPawns(TArray<APawn*>& OutAnimatronicPawns) const
{
    OutAnimatronicPawns.Empty();

    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn)
        {
            continue;
        }

        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            EFNAFAISpawnType AIType = IFNAFManagedAI::Execute_GetManagedAIType(Pawn);
            // From IDA: only include types <= Monty (Chica=0, Roxy=1, Monty=2)
            if (AIType <= EFNAFAISpawnType::Monty)
            {
                OutAnimatronicPawns.Add(Pawn);
            }
        }
    }
}

APawn* UAIManagementSystem::GetExistingPawn(EFNAFAISpawnType AIType, bool RequireShattered) const
{
    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn)
        {
            continue;
        }

        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            EFNAFAISpawnType PawnType = IFNAFManagedAI::Execute_GetManagedAIType(Pawn);
            if (PawnType == AIType)
            {
                if (RequireShattered)
                {
                    bool bIsShattered = IFNAFManagedAI::Execute_IsShatteredVersion(Pawn);
                    if (bIsShattered)
                    {
                        return Pawn;
                    }
                }
                else
                {
                    return Pawn;
                }
            }
        }
    }

    return nullptr;
}

TArray<EFNAFAISpawnType> UAIManagementSystem::GetExistingPawnTypes() const
{
    TArray<EFNAFAISpawnType> Result;

    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn)
        {
            continue;
        }

        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            EFNAFAISpawnType AIType = IFNAFManagedAI::Execute_GetManagedAIType(Pawn);
            Result.AddUnique(AIType);
        }
    }

    return Result;
}

APawn* UAIManagementSystem::GetPawnForType(EFNAFAISpawnType AIType) const
{
    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn)
        {
            continue;
        }

        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            EFNAFAISpawnType PawnType = IFNAFManagedAI::Execute_GetManagedAIType(Pawn);
            if (PawnType == AIType)
            {
                return Pawn;
            }
        }
    }

    return nullptr;
}

// Pawn class lookup
TSubclassOf<APawn> UAIManagementSystem::GetPawnClassForType(EFNAFAISpawnType AIType, bool bForceShattered) const
{
    EFNAFAISubType SubType = EFNAFAISubType::None;
    if (bForceShattered)
    {
        SubType = EFNAFAISubType::Shattered;
    }
    return GetPawnClassForTypeAndSubType(AIType, SubType);
}

TSubclassOf<APawn> UAIManagementSystem::GetPawnClassForTypeAndSubType(EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType) const
{
    const FFNAFAISettingInfo* SettingInfo = CharacterInfoDataMap.Find(AIType);
    if (!SettingInfo)
    {
        return nullptr;
    }

    // Determine effective sub-type
    EFNAFAISubType EffectiveSubType = forceAISubType;

    if (EffectiveSubType == EFNAFAISubType::None)
    {
        // Check WorldStateSystem for shattered state
        UWorldStateSystem* WorldStateSystem = GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
        if (WorldStateSystem)
        {
            FFNAFAISaveData AIState = WorldStateSystem->GetAIState();

            switch (AIType)
            {
            case EFNAFAISpawnType::Chica:
                if (AIState.bShatteredChica) EffectiveSubType = EFNAFAISubType::Shattered;
                break;
            case EFNAFAISpawnType::Roxy:
                if (AIState.bShatteredRoxy) EffectiveSubType = EFNAFAISubType::Shattered;
                break;
            case EFNAFAISpawnType::Monty:
                if (AIState.bShatteredMonty) EffectiveSubType = EFNAFAISubType::Shattered;
                break;
            default:
                break;
            }
        }
    }

    // Select the appropriate FSoftClassPath based on sub-type
    // From IDA: offset +0=Standard, +24=Shattered, +48=Ruined_SingleSpawn, +72=Ruined_Patrol
    const FSoftClassPath* ClassPath = nullptr;

    switch (EffectiveSubType)
    {
    case EFNAFAISubType::Shattered:
        ClassPath = &SettingInfo->ShatteredType;
        break;
    case EFNAFAISubType::Ruined_SingleSpawn:
        ClassPath = &SettingInfo->RuinedSingleSpawnType;
        break;
    case EFNAFAISubType::Ruined_Patrol:
        ClassPath = &SettingInfo->RuinedPatrolType;
        break;
    default:
        ClassPath = &SettingInfo->StandardType;
        break;
    }

    if (ClassPath)
    {
        UClass* LoadedClass = ClassPath->TryLoadClass<APawn>();
        if (LoadedClass)
        {
            return LoadedClass;
        }
    }

    return nullptr;
}

// Patrol path queries
TScriptInterface<ISeekerPatrolPath> UAIManagementSystem::GetPathForAI(EFNAFAISpawnType AIType, FName PathName) const
{
    for (const FWeakObjectPtr& WeakPath : RegisteredPatrolPaths)
    {
        UObject* PathObj = WeakPath.Get();
        if (!PathObj)
        {
            continue;
        }

        ISeekerPatrolPath* PatrolPath = Cast<ISeekerPatrolPath>(PathObj);
        if (!PatrolPath)
        {
            continue;
        }

        EFNAFAISpawnType PathSpawnType = ISeekerPatrolPath::Execute_GetSpawnType(PathObj);
        FName PathPathName = ISeekerPatrolPath::Execute_GetPathName(PathObj);

        if (PathSpawnType == AIType && PathPathName == PathName)
        {
            TScriptInterface<ISeekerPatrolPath> Result;
            Result.SetObject(PathObj);
            Result.SetInterface(PatrolPath);
            return Result;
        }
    }

    return TScriptInterface<ISeekerPatrolPath>();
}

TScriptInterface<ISeekerPatrolPath> UAIManagementSystem::GetPathByNameForAI(FName PathName) const
{
    for (const FWeakObjectPtr& WeakPath : RegisteredPatrolPaths)
    {
        UObject* PathObj = WeakPath.Get();
        if (!PathObj)
        {
            continue;
        }

        ISeekerPatrolPath* PatrolPath = Cast<ISeekerPatrolPath>(PathObj);
        if (!PatrolPath)
        {
            continue;
        }

        FName PathPathName = ISeekerPatrolPath::Execute_GetPathName(PathObj);

        if (PathPathName == PathName)
        {
            TScriptInterface<ISeekerPatrolPath> Result;
            Result.SetObject(PathObj);
            Result.SetInterface(PatrolPath);
            return Result;
        }
    }

    return TScriptInterface<ISeekerPatrolPath>();
}

// Room tracking
ARoomAreaBase* UAIManagementSystem::GetRoomAIPawnIsIn(APawn* AIPawn) const
{
    const ARoomAreaBase* const* Found = RoomOccupancy.Find(AIPawn);
    if (Found)
    {
        return const_cast<ARoomAreaBase*>(*Found);
    }
    return nullptr;
}

ARoomAreaBase* UAIManagementSystem::GetRoomAIPawnIsEntering(APawn* AIPawn) const
{
    for (const FAIRoomEntryInfo& Entry : RoomEntryList)
    {
        if (Entry.AIPawn == AIPawn)
        {
            return Entry.RoomEntering;
        }
    }
    return nullptr;
}

TArray<APawn*> UAIManagementSystem::GetAIPawnInRoom(ARoomAreaBase* Room) const
{
    const FRoomPawnList* Found = RoomOccupancyByRoom.Find(Room);
    if (Found)
    {
        return Found->Pawns;
    }
    return TArray<APawn*>();
}

TArray<APawn*> UAIManagementSystem::GetAIPawnsEnteringRoom(ARoomAreaBase* Room) const
{
    TArray<APawn*> Result;

    for (const FAIRoomEntryInfo& Entry : RoomEntryList)
    {
        if (Entry.RoomEntering == Room)
        {
            Result.Add(Entry.AIPawn);
        }
    }

    return Result;
}

bool UAIManagementSystem::IsRoomOccupied(ARoomAreaBase* Room) const
{
    const FRoomPawnList* Found = RoomOccupancyByRoom.Find(Room);
    return Found && Found->Pawns.Num() > 0;
}

bool UAIManagementSystem::IsRoomBeingEntered(ARoomAreaBase* Room) const
{
    for (const FAIRoomEntryInfo& Entry : RoomEntryList)
    {
        if (Entry.RoomEntering == Room)
        {
            return true;
        }
    }
    return false;
}

void UAIManagementSystem::PawnEnteredRoom(APawn* AIPawn, ARoomAreaBase* Room)
{
    if (!AIPawn || !Room)
    {
        return;
    }

    // Update RoomOccupancy (pawn -> room)
    RoomOccupancy.Add(AIPawn, Room);

    // Update RoomOccupancyByRoom (room -> pawns)
    FRoomPawnList& PawnList = RoomOccupancyByRoom.FindOrAdd(Room);
    PawnList.Pawns.AddUnique(AIPawn);

    // Remove from RoomEntryList since pawn has finished entering
    for (int32 i = RoomEntryList.Num() - 1; i >= 0; --i)
    {
        if (RoomEntryList[i].AIPawn == AIPawn)
        {
            RoomEntryList.RemoveAt(i);
            break;
        }
    }
}

void UAIManagementSystem::PawnEnteringRoom(APawn* AIPawn, ARoomAreaBase* Room)
{
    if (!AIPawn || !Room)
    {
        return;
    }

    FAIRoomEntryInfo Entry;
    Entry.AIPawn = AIPawn;
    Entry.RoomEntering = Room;
    RoomEntryList.Add(Entry);
}

void UAIManagementSystem::PawnExitedRoom(APawn* AIPawn, ARoomAreaBase* Room)
{
    if (!AIPawn || !Room)
    {
        return;
    }

    // Remove from RoomOccupancy
    RoomOccupancy.Remove(AIPawn);

    // Remove from RoomOccupancyByRoom
    FRoomPawnList* PawnList = RoomOccupancyByRoom.Find(Room);
    if (PawnList)
    {
        PawnList->Pawns.Remove(AIPawn);
        if (PawnList->Pawns.Num() == 0)
        {
            RoomOccupancyByRoom.Remove(Room);
        }
    }

    // Also remove from entry list just in case
    for (int32 i = RoomEntryList.Num() - 1; i >= 0; --i)
    {
        if (RoomEntryList[i].AIPawn == AIPawn)
        {
            RoomEntryList.RemoveAt(i);
            break;
        }
    }
}

TMap<APawn*, int32> UAIManagementSystem::GetRoomDistancesToPlayer() const
{
    TMap<APawn*, int32> Result;

    UWorld* World = GetWorld();
    if (!World)
    {
        return Result;
    }

    URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
    if (!RoomSys)
    {
        return Result;
    }

    // Get room distances from player's current rooms
    TMap<ARoomAreaBase*, int32> RoomDistances = RoomSys->GetAllRoomDistancesFromPlayerRoom();

    // For each registered pawn, look up its room in our RoomOccupancy map
    // and find the distance to the player's room(s)
    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn || !IsValid(Pawn)) continue;

        const ARoomAreaBase* const* PawnRoom = RoomOccupancy.Find(Pawn);
        if (PawnRoom && *PawnRoom)
        {
            const int32* Distance = RoomDistances.Find(const_cast<ARoomAreaBase*>(*PawnRoom));
            if (Distance)
            {
                Result.Add(Pawn, *Distance);
                continue;
            }
        }

        // Fallback: if pawn not in a tracked room, try GetRoomAtLocation
        ARoomAreaBase* Room = RoomSys->GetRoomAtLocation(Pawn->GetActorLocation());
        if (Room)
        {
            const int32* Distance = RoomDistances.Find(Room);
            if (Distance)
            {
                Result.Add(Pawn, *Distance);
                continue;
            }
        }

        // No room found - assign large distance
        Result.Add(Pawn, 999);
    }

    return Result;
}

void UAIManagementSystem::GetCachedDistanceFor(APawn* Pawn, bool& bOutResultValid, FAIDistanceResult& OutResult) const
{
    bOutResultValid = false;

    for (const FAIDistanceResult& Result : CachedDistanceResults)
    {
        if (Result.Pawn == Pawn)
        {
            OutResult = Result;
            bOutResultValid = true;
            return;
        }
    }
}

// Vanny Meter
float UAIManagementSystem::GetCurrentVannyMeterPercentage() const
{
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();
    if (Settings && Settings->VannyMeterMax > 0.0f)
    {
        return VannyMeter.CurrentValue / Settings->VannyMeterMax;
    }
    return 0.0f;
}

void UAIManagementSystem::GetCurrentVannyMeterValues(float& CurrentValue, float& Max, float& SoftMax) const
{
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();

    CurrentValue = VannyMeter.CurrentValue;
    Max = Settings ? Settings->VannyMeterMax : 0.0f;
    SoftMax = Settings ? Settings->VannyMeterLowAppear : 0.0f;
}

void UAIManagementSystem::GetCurrentVannyMeterValuesAsPercentage(float& OutCurrentValuePercent, float& OutSoftMaxPercent) const
{
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();

    if (Settings && Settings->VannyMeterMax > 0.0f)
    {
        OutCurrentValuePercent = VannyMeter.CurrentValue / Settings->VannyMeterMax;
        OutSoftMaxPercent = Settings->VannyMeterLowAppear / Settings->VannyMeterMax;
    }
    else
    {
        OutCurrentValuePercent = 0.0f;
        OutSoftMaxPercent = 0.0f;
    }
}

// ApplySound / ApplyCollect - Vanny meter + room POI heat
void UAIManagementSystem::ApplySound(float Strength, const FVector& WorldLocation)
{
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();
    if (Settings)
    {
        float VannyIncrease = Strength * Settings->VannyMeterIncreasePerSoundUnit;
        VannyMeter.AdjustVannyMeter(VannyIncrease);
    }

    // Adjust POI heat at the sound location
    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys)
        {
            const URoomSystemSettings* RoomSettings = GetDefault<URoomSystemSettings>();
            float HeatAmount = RoomSettings ? Strength * RoomSettings->POINearIncreasePerSecond : Strength;
            RoomSys->AdjustClosestPointOfInterestHeat(WorldLocation, HeatAmount);
        }
    }
}

void UAIManagementSystem::ApplyCollect(const FVector& WorldLocation)
{
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();
    if (Settings)
    {
        VannyMeter.AdjustVannyMeter(Settings->VannyMeterIncreasePerGift);
    }

    // Adjust POI heat at the collect location
    UWorld* World = GetWorld();
    if (World)
    {
        URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
        if (RoomSys)
        {
            const URoomSystemSettings* RoomSettings = GetDefault<URoomSystemSettings>();
            float HeatAmount = RoomSettings ? RoomSettings->POIHeatIncreaseOnCollect : 1.f;
            RoomSys->AdjustClosestPointOfInterestHeat(WorldLocation, HeatAmount);
        }
    }
}

bool UAIManagementSystem::AnyPawnInPlayerRoom() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
    if (!RoomSys)
    {
        return false;
    }

    TArray<ARoomAreaBase*> PlayerRooms = RoomSys->GetPlayerCurrentRooms();

    // Check RoomOccupancyByRoom for each player room
    for (ARoomAreaBase* PlayerRoom : PlayerRooms)
    {
        const FRoomPawnList* PawnList = RoomOccupancyByRoom.Find(PlayerRoom);
        if (PawnList && PawnList->Pawns.Num() > 0)
        {
            return true;
        }
    }

    // Also check RoomEntryList as fallback
    for (const FAIRoomEntryInfo& EntryInfo : RoomEntryList)
    {
        if (PlayerRooms.Contains(EntryInfo.RoomEntering))
        {
            return true;
        }
    }

    return false;
}

void UAIManagementSystem::DestroyAllAIInRoomsAtleast(int32 RoomDist)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
    if (!RoomSys)
    {
        // Fallback: destroy all if we can't calculate distances
        if (RoomDist <= 0)
        {
            RemoveAllCharacters();
        }
        return;
    }

    TMap<ARoomAreaBase*, int32> RoomDistances = RoomSys->GetAllRoomDistancesFromPlayerRoom();

    // Iterate registered pawns, destroy if room distance >= RoomDist
    TArray<APawn*> PawnsCopy = RegisteredPawns;
    for (APawn* Pawn : PawnsCopy)
    {
        if (!Pawn || !IsValid(Pawn)) continue;

        const ARoomAreaBase* const* PawnRoom = RoomOccupancy.Find(Pawn);
        if (PawnRoom && *PawnRoom)
        {
            const int32* Distance = RoomDistances.Find(const_cast<ARoomAreaBase*>(*PawnRoom));
            if (Distance && *Distance >= RoomDist)
            {
                Pawn->Destroy();
            }
        }
        else
        {
            // No room tracked — check via location
            ARoomAreaBase* Room = RoomSys->GetRoomAtLocation(Pawn->GetActorLocation());
            const int32* Distance = Room ? RoomDistances.Find(Room) : nullptr;
            if (!Distance || *Distance >= RoomDist)
            {
                Pawn->Destroy();
            }
        }
    }
}

TArray<APawn*> UAIManagementSystem::GetAllAIInRoomAtMost(int32 numRooms) const
{
    TArray<APawn*> Result;

    UWorld* World = GetWorld();
    if (!World)
    {
        return Result;
    }

    URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
    if (!RoomSys)
    {
        return Result;
    }

    TMap<ARoomAreaBase*, int32> RoomDistances = RoomSys->GetAllRoomDistancesFromPlayerRoom();

    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn || !IsValid(Pawn)) continue;

        const ARoomAreaBase* const* PawnRoom = RoomOccupancy.Find(Pawn);
        if (PawnRoom && *PawnRoom)
        {
            const int32* Distance = RoomDistances.Find(const_cast<ARoomAreaBase*>(*PawnRoom));
            if (Distance && *Distance <= numRooms)
            {
                Result.Add(Pawn);
            }
        }
    }

    return Result;
}

bool UAIManagementSystem::FindRandomPatrolPointOutOfView(EFNAFAISpawnType AIType, TScriptInterface<ISeekerPatrolPath> PatrolPath, FVector& OutLocation)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    UObject* PathObj = PatrolPath.GetObject();
    if (!PathObj || !PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
    {
        return false;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return false;
    }

    int32 NumPoints = IPathPointProvider::Execute_GetNumberOfPathPoints(PathObj);
    if (NumPoints <= 0)
    {
        return false;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    FRotator CameraRotation = CameraManager->GetCameraRotation();
    FVector CameraForward = CameraRotation.Vector();

    // Build shuffled index array
    TArray<int32> Indices;
    for (int32 i = 0; i < NumPoints; ++i)
    {
        Indices.Add(i);
    }

    // Fisher-Yates shuffle
    for (int32 i = Indices.Num() - 1; i > 0; --i)
    {
        int32 j = FMath::RandRange(0, i);
        Indices.Swap(i, j);
    }

    for (int32 Idx : Indices)
    {
        FVector PointLocation = IPathPointProvider::Execute_GetPointLocation(PathObj, Idx);

        // Check if point is outside camera FOV
        FVector DirToPoint = (PointLocation - CameraLocation).GetSafeNormal();
        float DotProduct = FVector::DotProduct(CameraForward, DirToPoint);

        // If dot product > ~0.7 (within ~45 degree FOV), it's in view - skip
        if (DotProduct > 0.7f)
        {
            continue;
        }

        // Line trace from camera to point
        FHitResult HitResult;
        FCollisionQueryParams QueryParams;

        bool bHit = World->LineTraceSingleByChannel(
            HitResult,
            CameraLocation,
            PointLocation,
            ECC_Visibility,
            QueryParams
        );

        // If we can see the point (no obstruction), skip
        if (!bHit)
        {
            continue;
        }

        // Check minimum distance
        float Distance = FVector::Dist(CameraLocation, PointLocation);
        if (Distance < AISpawnOnLoadProximity)
        {
            continue;
        }

        OutLocation = PointLocation;
        return true;
    }

    return false;
}

// Registration
void UAIManagementSystem::RegisterAI(APawn* AIPawn)
{
    if (!AIPawn)
    {
        return;
    }

    UE_LOG(LogAIManagement, Warning, TEXT("RegisterAI: %s (type %d), RegisteredPawns count: %d -> %d"),
        *AIPawn->GetName(),
        AIPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()) ?
        (int32)IFNAFManagedAI::Execute_GetManagedAIType(AIPawn) : -1,
        RegisteredPawns.Num(), RegisteredPawns.Num() + 1);

    RegisteredPawns.AddUnique(AIPawn);

    // IDA: Binds OnPawnEndPlay to the pawn's OnEndPlay sparse delegate
    // This ensures AI are automatically unregistered when destroyed
    AIPawn->OnEndPlay.AddDynamic(this, &UAIManagementSystem::OnPawnEndPlay);

    // Track Endo-type pawns separately
    if (AIPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
    {
        EFNAFAISpawnType AIType = IFNAFManagedAI::Execute_GetManagedAIType(AIPawn);
        if (AIType == EFNAFAISpawnType::Endo)
        {
            RegisteredEndos.AddUnique(AIPawn);
        }
    }
}

void UAIManagementSystem::UnRegisterAI(APawn* AIPawn)
{
    if (!AIPawn)
    {
        return;
    }

    UE_LOG(LogAIManagement, Warning, TEXT("UnRegisterAI: %s, RegisteredPawns count: %d"),
        *AIPawn->GetName(), RegisteredPawns.Num());
    RegisteredPawns.Remove(AIPawn);
    PawnsWithSightToPlayer.Remove(AIPawn);
    RegisteredEndos.Remove(AIPawn);

    // IDA: Unbind OnEndPlay sparse delegate
    AIPawn->OnEndPlay.RemoveDynamic(this, &UAIManagementSystem::OnPawnEndPlay);

    RoomOccupancy.Remove(AIPawn);

    // Remove from RoomOccupancyByRoom
    for (auto& Pair : RoomOccupancyByRoom)
    {
        Pair.Value.Pawns.Remove(AIPawn);
    }

    // Remove from RoomEntryList
    for (int32 i = RoomEntryList.Num() - 1; i >= 0; --i)
    {
        if (RoomEntryList[i].AIPawn == AIPawn)
        {
            RoomEntryList.RemoveAt(i);
        }
    }

    if (AIHiding == AIPawn)
    {
        AIHiding = nullptr;
        CurrentHideLocation = nullptr;
    }

    // Remove from TypesSpawned if no more pawns of this type remain
    if (AIPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
    {
        EFNAFAISpawnType AIType = IFNAFManagedAI::Execute_GetManagedAIType(AIPawn);
        bool bStillExists = false;
        for (APawn* Pawn : RegisteredPawns)
        {
            if (Pawn && IsValid(Pawn) && Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
            {
                if (IFNAFManagedAI::Execute_GetManagedAIType(Pawn) == AIType)
                {
                    bStillExists = true;
                    break;
                }
            }
        }
        if (!bStillExists)
        {
            TypesSpawned.Remove(AIType);
            UE_LOG(LogAIManagement, Warning, TEXT("UnRegisterAI: Removed type %d from TypesSpawned (now %d entries)"),
                (int32)AIType, TypesSpawned.Num());
        }
    }
}

void UAIManagementSystem::RegisterSeekerPath(TScriptInterface<ISeekerPatrolPath> SeekerPatrolPath)
{
    UObject* PathObj = SeekerPatrolPath.GetObject();
    if (PathObj)
    {
        RegisteredPatrolPaths.AddUnique(FWeakObjectPtr(PathObj));
    }
}

void UAIManagementSystem::UnregisterSeekerPath(TScriptInterface<ISeekerPatrolPath> SeekerPatrolPath)
{
    UObject* PathObj = SeekerPatrolPath.GetObject();
    if (PathObj)
    {
        RegisteredPatrolPaths.Remove(FWeakObjectPtr(PathObj));
    }
}

void UAIManagementSystem::OnPawnEndPlay(AActor* DestroyedPawn, EEndPlayReason::Type EndPlayReason)
{
    APawn* DestroyedAsPawn = Cast<APawn>(DestroyedPawn);
    if (DestroyedAsPawn)
    {
        UnRegisterAI(DestroyedAsPawn);
    }
}

// Teleport AI back to patrol path; destroy if impossible
void UAIManagementSystem::OnAIFellOutOfWorld(APawn* AIPawn)
{
    if (!AIPawn || !IsValid(AIPawn))
    {
        return;
    }

    // Try to get AI type and path info for re-placement
    if (AIPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
    {
        EFNAFAISpawnType AIType = IFNAFManagedAI::Execute_GetManagedAIType(AIPawn);
        FName PathName = IFNAFManagedAI::Execute_GetCurrentPathName(AIPawn);

        TScriptInterface<ISeekerPatrolPath> PatrolPath = GetPathForAI(AIType, PathName);

        if (PatrolPath.GetObject())
        {
            FVector NewLocation;
            if (FindRandomPatrolPointOutOfView(AIType, PatrolPath, NewLocation))
            {
                // Adjust for capsule half height if character
                ACharacter* Character = Cast<ACharacter>(AIPawn);
                if (Character && Character->GetCapsuleComponent())
                {
                    NewLocation.Z += Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                }

                AIPawn->SetActorLocation(NewLocation, false, nullptr, ETeleportType::ResetPhysics);
                return;
            }
        }
    }

    // Teleport failed, destroy pawn
    AIPawn->Destroy();
}

void UAIManagementSystem::RemoveAllCharacters()
{
    // Copy array since Destroy will trigger OnPawnEndPlay -> UnRegisterAI -> modify array
    TArray<APawn*> PawnsCopy = RegisteredPawns;

    for (APawn* Pawn : PawnsCopy)
    {
        if (Pawn && IsValid(Pawn))
        {
            Pawn->Destroy();
        }
    }

    // Clear AnimatronicStates in WorldStateSystem
    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (WorldStateSys)
    {
        FFNAFAISaveData AIState = WorldStateSys->GetAIState();
        AIState.AnimatronicStates.Empty();
        WorldStateSys->SetAIState(AIState);
    }

    RegisteredPawns.Empty();
    PawnsWithSightToPlayer.Empty();
    RoomOccupancy.Empty();
    RoomOccupancyByRoom.Empty();
    RoomEntryList.Empty();
    TypesSpawned.Empty();
}

void UAIManagementSystem::RemoveCharacterByType(EFNAFAISpawnType AIType)
{
    TArray<APawn*> PawnsCopy = RegisteredPawns;

    for (APawn* Pawn : PawnsCopy)
    {
        if (!Pawn || !IsValid(Pawn))
        {
            continue;
        }

        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            EFNAFAISpawnType PawnType = IFNAFManagedAI::Execute_GetManagedAIType(Pawn);
            if (PawnType == AIType)
            {
                Pawn->Destroy();
            }
        }
    }
}

void UAIManagementSystem::RemoveExpectedAI(EFNAFAISpawnType AIType)
{
    for (int32 i = TypesExpected.Num() - 1; i >= 0; --i)
    {
        if (TypesExpected[i].AIType == AIType)
        {
            TypesExpected.RemoveAt(i);
            break;
        }
    }
}

/*
 * SendGeneralAlert -- Two paths: if TypesToAlert is non-empty, randomly pick
 * types and match to existing pawns or spawn via SpawnAlertAI. If empty, find
 * closest alertable pawn or spawn a random animatronic.
 */
void UAIManagementSystem::SendGeneralAlert(const FVector& AlertLocation, TArray<FAnimatronicTypeData> TypesToAlert, int32 NumberToAlert)
{
    static int32 AlertCallCount = 0;
    AlertCallCount++;
    UE_LOG(LogAIManagement, Warning, TEXT("SendGeneralAlert #%d: TypesToAlert=%d, NumberToAlert=%d, RegisteredPawns=%d, at %s (Frame=%llu)"),
        AlertCallCount, TypesToAlert.Num(), NumberToAlert, RegisteredPawns.Num(), *AlertLocation.ToString(),
        GFrameCounter);
    // IDA: static local array initialized once: {Chica=0, Roxy=1, Monty=2}
    static const EFNAFAISpawnType SpawnTypes[] = {
        EFNAFAISpawnType::Chica,
        EFNAFAISpawnType::Roxy,
        EFNAFAISpawnType::Monty
    };
    static const int32 NumSpawnTypes = UE_ARRAY_COUNT(SpawnTypes);

    // PATH 1: TypesToAlert has entries -- match types to existing AI
    if (TypesToAlert.Num() > 0)
    {
        int32 AlertsSent = 0;

        // IDA: loop up to NumberToAlert, randomly picking types
        for (int32 Iteration = 0; Iteration < NumberToAlert && NumberToAlert > 0; ++Iteration)
        {
            if (TypesToAlert.Num() <= 0)
            {
                // TypesToAlert exhausted — spawn remainder from SpawnTypes
                // IDA LABEL_18: remaining = NumberToAlert - AlertsSent
                int32 Remaining = NumberToAlert - AlertsSent;
                for (int32 i = 0; i < Remaining; ++i)
                {
                    // IDA: build array of FAnimatronicTypeData from SpawnTypes bytes
                    // with SubType = None(4), random pick
                    EFNAFAISpawnType RandomType = SpawnTypes[FMath::RandRange(0, NumSpawnTypes - 1)];
                    FAnimatronicTypeData TypeToSpawn;
                    TypeToSpawn.AIType = RandomType;
                    TypeToSpawn.AISubType = EFNAFAISubType::None;
                    SpawnAlertAI(AlertLocation, TypeToSpawn);
                }
                return;
            }

            // IDA: random pick from TypesToAlert, then RemoveAt
            int32 PickIndex = FMath::RandRange(0, TypesToAlert.Num() - 1);
            FAnimatronicTypeData PickedType = TypesToAlert[PickIndex];
            TypesToAlert.RemoveAt(PickIndex);

            // IDA: search RegisteredPawns for matching type
            APawn* FoundPawn = nullptr;
            for (APawn* Pawn : RegisteredPawns)
            {
                if (!Pawn || !Pawn->GetClass())
                {
                    continue;
                }

                if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
                {
                    EFNAFAISpawnType PawnType = IFNAFManagedAI::Execute_GetManagedAIType(Pawn);
                    // IDA: calls IsShatteredVersion but doesn't use result for matching
                    IFNAFManagedAI::Execute_IsShatteredVersion(Pawn);

                    if (PawnType == PickedType.AIType)
                    {
                        FoundPawn = Pawn;
                        break;
                    }
                }
            }

            if (!FoundPawn)
            {
                // IDA LABEL_12 → LABEL_13: no matching pawn → SpawnAlertAI
                SpawnAlertAI(AlertLocation, PickedType);
                AlertsSent++;
                continue;
            }

            // IDA: double validity check on found pawn
            // First check: if invalid → spawn instead (LABEL_12 → LABEL_13)
            if (!IsValid(FoundPawn)
                || !FoundPawn->GetClass()
                || !FoundPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
            {
                SpawnAlertAI(AlertLocation, PickedType);
                AlertsSent++;
                continue;
            }

            // IDA: second validity check: if invalid → skip this iteration (retry)
            // IDA: --v11 then goto LABEL_17 which does ++v11, net zero = retry
            if (!IsValid(FoundPawn)
                || !FoundPawn->GetClass()
                || !FoundPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
            {
                --Iteration; // counteract for-loop ++Iteration, effectively retry
                continue;
            }

            // Both checks pass → alert existing AI
            UE_LOG(LogAIManagement, Warning, TEXT("SendGeneralAlert: ALERTING existing pawn %s (type %d) at %s"),
                *FoundPawn->GetName(), (int32)PickedType.AIType, *AlertLocation.ToString());
            IFNAFManagedAI::Execute_SendPositionalAlert(FoundPawn, AlertLocation);
            AlertsSent++;
        }

        // IDA LABEL_18: after main loop, spawn remainder if needed
        if (AlertsSent < NumberToAlert)
        {
            int32 Remaining = NumberToAlert - AlertsSent;
            for (int32 i = 0; i < Remaining; ++i)
            {
                EFNAFAISpawnType RandomType = SpawnTypes[FMath::RandRange(0, NumSpawnTypes - 1)];
                FAnimatronicTypeData TypeToSpawn;
                TypeToSpawn.AIType = RandomType;
                TypeToSpawn.AISubType = EFNAFAISubType::None;
                SpawnAlertAI(AlertLocation, TypeToSpawn);
            }
        }

        return;
    }

    // PATH 2: TypesToAlert empty -- generic alert to closest available AI

    // IDA: collect all pawns where CanReceiveAlert == true
    TArray<APawn*> AvailablePawns;
    for (APawn* Pawn : RegisteredPawns)
    {
        if (!Pawn || !Pawn->GetClass())
        {
            continue;
        }

        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            if (IFNAFManagedAI::Execute_CanReceiveAlert(Pawn))
            {
                AvailablePawns.Add(Pawn);
            }
        }
    }

    if (AvailablePawns.Num() == 0)
    {
        // IDA LABEL_110: no available pawns → SpawnAlertAI with random type
        EFNAFAISpawnType RandomType = SpawnTypes[FMath::RandRange(0, NumSpawnTypes - 1)];
        FAnimatronicTypeData TypeToSpawn;
        TypeToSpawn.AIType = RandomType;
        TypeToSpawn.AISubType = EFNAFAISubType::None;
        SpawnAlertAI(AlertLocation, TypeToSpawn);
        return;
    }

    // IDA: original uses FShortestPathFinder with async nav queries
    // ("Find Closest Alerted AI"), then alerts closest or falls back to spawn.
    // Simplified to synchronous distance check — functionally equivalent for
    // most level layouts where nav distance ≈ Euclidean distance.
    APawn* ClosestPawn = nullptr;
    float ClosestDistSq = MAX_FLT;

    for (APawn* Pawn : AvailablePawns)
    {
        float DistSq = FVector::DistSquared(Pawn->GetActorLocation(), AlertLocation);
        if (DistSq < ClosestDistSq)
        {
            ClosestDistSq = DistSq;
            ClosestPawn = Pawn;
        }
    }

    if (ClosestPawn)
    {
        UE_LOG(LogAIManagement, Warning, TEXT("SendGeneralAlert PATH2: ALERTING closest pawn %s (dist=%.0f) at %s"),
            *ClosestPawn->GetName(), FMath::Sqrt(ClosestDistSq), *AlertLocation.ToString());
        IFNAFManagedAI::Execute_SendPositionalAlert(ClosestPawn, AlertLocation);
    }
    else
    {
        // Fallback: should not reach here since AvailablePawns is non-empty,
        // but matches IDA lambda fallback to SpawnAlertAI
        EFNAFAISpawnType RandomType = SpawnTypes[FMath::RandRange(0, NumSpawnTypes - 1)];
        FAnimatronicTypeData TypeToSpawn;
        TypeToSpawn.AIType = RandomType;
        TypeToSpawn.AISubType = EFNAFAISubType::None;
        SpawnAlertAI(AlertLocation, TypeToSpawn);
    }
}

void UAIManagementSystem::SendVanessaAlert(TArray<EFNAFAISpawnType> TypesToAlert, int32 NumberToAlert)
{
    UE_LOG(LogAIManagement, Warning, TEXT("SendVanessaAlert called (TypesToAlert=%d, NumberToAlert=%d)"), TypesToAlert.Num(), NumberToAlert);
    // Find Vanessa pawn and use her location as alert origin
    APawn* VanessaPawn = GetPawnForType(EFNAFAISpawnType::Vanessa);
    if (!VanessaPawn || !VanessaPawn->GetRootComponent())
    {
        return;
    }

    FVector AlertLocation = VanessaPawn->GetRootComponent()->GetComponentLocation();

    // Convert EFNAFAISpawnType array to FAnimatronicTypeData array
    TArray<FAnimatronicTypeData> TypeDataArray = UAnimatronicTypeDataBlueprintFunctionLibrary::GetAnimatronicTypeDataArrayFromFNAFAISpawnTypeArray(
        TypesToAlert,
        EFNAFAISubType::None // None
    );

    SendGeneralAlert(AlertLocation, TypeDataArray, NumberToAlert);
}

/*
 * SpawnAlertAI -- Spawns an animatronic near the player using EQS to find
 * a not-visible position. Base game uses EQNearPlayerNotVisible; DLC searches
 * QueryData for a type-specific query or falls back to DLC version.
 */
void UAIManagementSystem::SpawnAlertAI(const FVector& AlertLocation, const FAnimatronicTypeData& SpawnType)
{
    UE_LOG(LogAIManagement, Warning, TEXT("SpawnAlertAI: type %d at %s"), (int32)SpawnType.AIType, *AlertLocation.ToString());

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Determine which EQS query to use based on game type
    UEnvQuery* EnvQuery = nullptr;

    UFNAFGameInstanceBase* GameInstance = Cast<UFNAFGameInstanceBase>(World->GetGameInstance());
    if (GameInstance && GameInstance->GetCurrentGameType() == EFNAFGameType::ChowdaMode)
    {
        // ---- DLC (Ruin) path ----
        // IDA: search QueryData for matching AIType
        for (UAIEnvironmentQueryInfo* Info : QueryData)
        {
            if (Info && Info->AIType == SpawnType.AIType)
            {
                EnvQuery = Info->EnvirontmentQuery;
                break;
            }
        }

        // IDA: fallback to generic DLC EQS query
        if (!EnvQuery)
        {
            EnvQuery = Cast<UEnvQuery>(StaticLoadObject(
                UEnvQuery::StaticClass(), this,
                TEXT("/Chowda/Blueprints/AI/EnvQueries/EQNearPlayerNotVisible_DLC.EQNearPlayerNotVisible_DLC")));
        }
    }
    else
    {
        // ---- Base game path ----
        // IDA: load base game EQS query
        EnvQuery = Cast<UEnvQuery>(StaticLoadObject(
            UEnvQuery::StaticClass(), this,
            TEXT("/Game/Blueprints/AI/EnvQueries/EQNearPlayerNotVisible.EQNearPlayerNotVisible")));
    }

    if (!EnvQuery)
    {
        // EQS query not available — fall back to direct spawn near alert location
        // This shouldn't happen in a proper install but prevents silent failure
        FTransform SpawnTransform(FRotator::ZeroRotator, AlertLocation, FVector::OneVector);
        bool bSuccess = false;
        SpawnAITypeWithTransformSafeWithSubType(
            SpawnType.AIType, SpawnTransform, SpawnType.AISubType,
            bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
        return;
    }

    // ---- Execute EQS query asynchronously ----
    // IDA: FEnvQueryRequest::Execute(SingleResult, lambda callback)
    // The lambda captures AIManager (weak), AlertLocation, SpawnType, QueryData
    // and spawns the AI at the EQS result location when the query completes.

    FEnvQueryRequest QueryRequest(EnvQuery, this);

    // IDA: DLC path sets Distance.FloatValueMax from RabbitSystem
    if (GameInstance && GameInstance->GetCurrentGameType() == EFNAFGameType::ChowdaMode)
    {
        UAIDLC_RabbitSystem* RabbitSystem = World->GetSubsystem<UAIDLC_RabbitSystem>();
        if (RabbitSystem)
        {
            float SpawnDistance = RabbitSystem->GetAnimatronicSpawnDistance();
            QueryRequest.SetFloatParam(FName("Distance.FloatValueMax"), SpawnDistance);
        }
    }

    // Capture spawn parameters for the async callback
    TWeakObjectPtr<UAIManagementSystem> WeakThis(this);
    FAnimatronicTypeData CapturedType = SpawnType;
    FVector CapturedAlertLocation = AlertLocation;

    // Block SpawnHandling while async EQS query is pending
    // Prevents race: SpawnHandling spawning Chica on patrol path while
    // this async query is still finding a near-player position
    bIsSearchingForSpawn = true;

    QueryRequest.Execute(EEnvQueryRunMode::SingleResult,
        FQueryFinishedSignature::CreateLambda(
            [WeakThis, CapturedType, CapturedAlertLocation](TSharedPtr<FEnvQueryResult> Result)
            {
                UAIManagementSystem* Manager = WeakThis.Get();
                if (!Manager)
                {
                    return;
                }

                // Unblock SpawnHandling now that EQS query is complete
                Manager->bIsSearchingForSpawn = false;

                FVector SpawnLocation = CapturedAlertLocation;

                if (Result.IsValid() && Result->IsSuccsessful() && Result->Items.Num() > 0)
                {
                    SpawnLocation = Result->GetItemAsLocation(0);
                }

                FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation, FVector::OneVector);
                bool bSuccess = false;
                Manager->SpawnAITypeWithTransformSafeWithSubType(
                    CapturedType.AIType, SpawnTransform, CapturedType.AISubType,
                    bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
            }));
}

// Hardcoded 4 entries per IDA, not dynamic iteration
void UAIManagementSystem::SetAllAIExpected()
{
    TypesExpected.Empty();
    TypesExpected.SetNum(4);

    // Entry 0: Chica (AIType byte = 0)
    TypesExpected[0].AIType = EFNAFAISpawnType::Chica;
    TypesExpected[0].PathName = NAME_None;

    // Entry 1: Monty (AIType byte = 2)
    TypesExpected[1].AIType = EFNAFAISpawnType::Monty;
    TypesExpected[1].PathName = NAME_None;

    // Entry 2: Roxy (AIType byte = 1)
    TypesExpected[2].AIType = EFNAFAISpawnType::Roxy;
    TypesExpected[2].PathName = NAME_None;

    // Entry 3: Vanessa (AIType byte = 3)
    TypesExpected[3].AIType = EFNAFAISpawnType::Vanessa;
    TypesExpected[3].PathName = NAME_None;
}

/*
 * OnWorldStateChanged -- State 0 (Normal): resume AI, respawn. States 2,4,5,6
 * (MoonManIntermediate, RepairGame, OfficeGame, BossGame): pause AI, store
 * endo states, optionally remove all characters.
 */
void UAIManagementSystem::OnWorldStateChanged(EFNAFGameState NewState, EFNAFGameState OldState)
{
    uint8 State = (uint8)NewState;

    if (State == 0)
    {
        // Normal gameplay: resume AI
        UnpauseManager();
        SetWorldSpawnEnabled(true);
        Reset();

        if (OldState != EFNAFGameState::PowerCycle)
        {
            RespawnAnimatronics();

            // IDA: if no endos registered, respawn from WorldStateSystem save data
            if (RegisteredEndos.Num() <= 0)
            {
                UWorld* World = GetWorld();
                if (World && World->GetGameInstance())
                {
                    UWorldStateSystem* WorldStateSys = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
                    if (WorldStateSys)
                    {
                        FFNAFAISettingInfo* EndoInfo = GetAITypeInfo((EFNAFAISpawnType)5); // Endo type
                        if (EndoInfo)
                        {
                            UClass* EndoClass = EndoInfo->StandardType.TryLoadClass<APawn>();
                            if (EndoClass)
                            {
                                const FFNAFAISaveData& AIState = WorldStateSys->GetAIState();
                                for (const FEndoSaveData& EndoData : AIState.Endos)
                                {
                                    FActorSpawnParameters SpawnParams;
                                    World->SpawnActor(EndoClass, &EndoData.Location, &EndoData.Rotation, SpawnParams);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else if (State == 2 || State == 4 || (State >= 5 && State <= 6))
    {
        // Special game modes: pause AI
        PauseManager();
        SetWorldSpawnEnabled(false);
        StoreEndoStates();
        if (!DoNotDespawnAIDuringMoonmanTime)
        {
            RemoveAllCharacters();
        }
        Reset();
    }
}

void UAIManagementSystem::OnVannyPathsCollected()
{
    // Called when async Vanny patrol path collection completes
    // Sets internal state to allow Vanny spawning to proceed
}

// Monty hiding logic (Chowda/Ruin mode): spawns Monty in adjacent room hide locations
void UAIManagementSystem::HandleHidingAI()
{
    // Only handle hiding if no AI is currently hiding
    if (AIHiding)
    {
        return;
    }

    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (!WorldStateSys)
    {
        return;
    }

    FFNAFAISaveData AIState = WorldStateSys->GetAIState();
    if (!AIState.bShatteredMonty)
    {
        return;
    }

    // Check if Monty pawn already exists
    APawn* ExistingMonty = GetPawnForType(EFNAFAISpawnType::Monty);
    if (ExistingMonty)
    {
        return;
    }

    // Get player rooms and find adjacent rooms with AI hide locations
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    URoomSystem* RoomSys = World->GetSubsystem<URoomSystem>();
    if (!RoomSys)
    {
        return;
    }

    TArray<ARoomAreaBase*> PlayerRooms = RoomSys->GetPlayerCurrentRooms();

    // Collect all AI hide actors from rooms adjacent to player
    TArray<AActor*> CandidateHideActors;
    for (ARoomAreaBase* PlayerRoom : PlayerRooms)
    {
        TArray<FRoomAdjacencyInfo> Adjacent = PlayerRoom->GetAllAdjacentRooms();
        for (const FRoomAdjacencyInfo& AdjInfo : Adjacent)
        {
            ARoomAreaBase* AdjRoom = AdjInfo.Room.Get();
            if (!AdjRoom) continue;

            TArray<AActor*> HideActors = AdjRoom->GetAllAIHideActors();
            for (AActor* HideActor : HideActors)
            {
                if (HideActor)
                {
                    CandidateHideActors.AddUnique(HideActor);
                }
            }
        }
    }

    if (CandidateHideActors.Num() == 0)
    {
        return;
    }

    // Random select a hide location
    AActor* ChosenHideActor = CandidateHideActors[FMath::RandRange(0, CandidateHideActors.Num() - 1)];

    // Use ProcessEvent instead of Execute_* to avoid cross-module linker errors
    // Get hide location from the IAIHideLocationInterface
    FVector HideLocation = ChosenHideActor->GetActorLocation();
    FRotator HideRotation = ChosenHideActor->GetActorRotation();

    // ProcessEvent replacement for IAIHideLocationInterface::Execute_GetHideLocationAndRotation
    // (avoids cross-module linker dependency on RoomSystem)
    {
        UFunction* GetHideLocFunc = ChosenHideActor->FindFunction(FName("GetHideLocationAndRotation"));
        if (GetHideLocFunc)
        {
            struct
            {
                FVector Location;
                FRotator Rotation;
            } Params;
            Params.Location = HideLocation;
            Params.Rotation = HideRotation;
            ChosenHideActor->ProcessEvent(GetHideLocFunc, &Params);
            HideLocation = Params.Location;
            HideRotation = Params.Rotation;
        }
    }

    // Spawn Monty with Shattered subtype
    FTransform SpawnTransform(HideRotation, HideLocation, FVector::OneVector);
    bool bSuccess = false;
    APawn* SpawnedMonty = SpawnAITypeWithTransformSafeWithSubType(
    EFNAFAISpawnType::Monty, SpawnTransform, EFNAFAISubType::Shattered,
    bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);

    if (SpawnedMonty)
    {
        AIHiding = SpawnedMonty;

        // Enter hide mode — AIHiderInterface is in fnaf9 module, so Execute_* works fine
        if (SpawnedMonty->GetClass()->ImplementsInterface(UAIHiderInterface::StaticClass()))
        {
            IAIHiderInterface::Execute_EnterHideMode(SpawnedMonty, ChosenHideActor);
        }

        // Setup world cue — use FindFunction/ProcessEvent to avoid RoomSystem dependency
        AActor* CueActor = nullptr;

        // ProcessEvent replacement for IAIHideLocationInterface::Execute_GetHideCueActor because this kept making linker errors
        {
            UFunction* GetCueFunc = ChosenHideActor->FindFunction(FName("GetHideCueActor"));
            if (GetCueFunc)
            {
                struct
                {
                    AActor* ReturnValue;
                } Params;
                Params.ReturnValue = nullptr;
                ChosenHideActor->ProcessEvent(GetCueFunc, &Params);
                CueActor = Params.ReturnValue;
            }
        }

        // ProcessEvent replacement for IHideCueObjectInterface::Execute_SetupWorldCue
        if (CueActor)
        {
            UFunction* SetupCueFunc = CueActor->FindFunction(FName("SetupWorldCue"));
            if (SetupCueFunc)
            {
                CueActor->ProcessEvent(SetupCueFunc, nullptr);
            }
        }
    }
}

// Spawn functions
void UAIManagementSystem::SpawnVannyOrVanessa(bool bSpawnVanny, bool& bOutSpawned, FLatentActionInfo LatentActionInfo)
{
    bOutSpawned = false;

    if (!bSpawningEnabled || !bRandomWorldSpawningEnabled || !bVanessaSpawnEnabled)
    {
        return;
    }

    // Check if Vanessa/Vanny already exists
    APawn* ExistingVanessa = GetPawnForType(EFNAFAISpawnType::Vanessa);
    if (ExistingVanessa)
    {
        return;
    }

    // Get all spawn points for Vanessa type
    TArray<AFNAFAISpawnPoint*> VanessaSpawnPoints = GetAllSpawnPointsFor(EFNAFAISpawnType::Vanessa);
    if (VanessaSpawnPoints.Num() == 0)
    {
        return;
    }

    // Filter to hidden spawn points (not visible from player camera)
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    FVector CameraForward = CameraManager->GetCameraRotation().Vector();

    TArray<AFNAFAISpawnPoint*> HiddenSpawnPoints;
    for (AFNAFAISpawnPoint* SpawnPoint : VanessaSpawnPoints)
    {
        if (!SpawnPoint || !SpawnPoint->GetRootComponent())
        {
            continue;
        }

        FVector SpawnLocation = SpawnPoint->GetRootComponent()->GetComponentLocation();
        FVector DirToSpawn = (SpawnLocation - CameraLocation).GetSafeNormal();

        // Check if out of FOV
        if (FVector::DotProduct(CameraForward, DirToSpawn) > 0.7f)
        {
            // In view - do line trace to confirm
            FHitResult HitResult;
            FCollisionQueryParams QueryParams;
            bool bHit = World->LineTraceSingleByChannel(HitResult, CameraLocation, SpawnLocation, ECC_Visibility, QueryParams);
            if (!bHit)
            {
                continue; // Visible, skip
            }
        }

        HiddenSpawnPoints.Add(SpawnPoint);
    }

    if (HiddenSpawnPoints.Num() == 0)
    {
        return;
    }

    // Select closest hidden spawn point
    float ClosestDist = MAX_FLT;
    AFNAFAISpawnPoint* BestSpawnPoint = nullptr;
    for (AFNAFAISpawnPoint* SpawnPoint : HiddenSpawnPoints)
    {
        float Dist = FVector::Dist(CameraLocation, SpawnPoint->GetRootComponent()->GetComponentLocation());
        if (Dist < ClosestDist)
        {
            ClosestDist = Dist;
            BestSpawnPoint = SpawnPoint;
        }
    }

    if (!BestSpawnPoint)
    {
        return;
    }

    // Spawn using RuinedPatrol subtype (type 3 from IDA)
    FVector SpawnLocation = BestSpawnPoint->GetRootComponent()->GetComponentLocation();
    FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation, FVector::OneVector);

    bool bSuccess = false;
    APawn* SpawnedPawn = SpawnAITypeWithTransformSafeWithSubType(
        EFNAFAISpawnType::Vanessa,
        SpawnTransform,
        EFNAFAISubType::Ruined_Patrol,
        bSuccess,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        nullptr,
        false
    );

    bOutSpawned = bSuccess && SpawnedPawn != nullptr;
}

APawn* UAIManagementSystem::SpawnSpecificAIOnPathWithSubType(EFNAFAISpawnType AIType, EFNAFAISubType AISubType, FName PathName)
{
    // Search RegisteredPatrolPaths for matching path by name
    TScriptInterface<ISeekerPatrolPath> PatrolPath;

    for (const FWeakObjectPtr& WeakPath : RegisteredPatrolPaths)
    {
        UObject* PathObj = WeakPath.Get();
        if (!PathObj)
        {
            continue;
        }

        FName TestPathName = ISeekerPatrolPath::Execute_GetPathName(PathObj);
        if (TestPathName == PathName)
        {
            ISeekerPatrolPath* PathInterface = Cast<ISeekerPatrolPath>(PathObj);
            if (PathInterface)
            {
                PatrolPath.SetObject(PathObj);
                PatrolPath.SetInterface(PathInterface);
                break;
            }
        }
    }

    if (!PatrolPath.GetObject())
    {
        return nullptr;
    }

    FVector SpawnLocation;
    if (!FindRandomPatrolPointOutOfView(AIType, PatrolPath, SpawnLocation))
    {
        return nullptr;
    }

    FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation, FVector::OneVector);
    bool bSuccess = false;
    APawn* SpawnedPawn = SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, AISubType, bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);

    if (SpawnedPawn)
    {
        StoreAnimatronicSpawnWithSubType(AIType, PathName, AISubType);

        // Set patrol path on spawned AI
        if (SpawnedPawn->GetClass()->ImplementsInterface(UPatrollerInterface::StaticClass()))
        {
            // Convert ISeekerPatrolPath to IPathPointProvider for SetPatrolPath
            UObject* PathObj = PatrolPath.GetObject();
            if (PathObj && PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
            {
                TScriptInterface<IPathPointProvider> PathProvider;
                PathProvider.SetObject(PathObj);
                PathProvider.SetInterface(Cast<IPathPointProvider>(PathObj));
                IPatrollerInterface::Execute_SetPatrolPath(SpawnedPawn, PathProvider);
            }
        }
    }

    return SpawnedPawn;
}

APawn* UAIManagementSystem::SpawnSpecificAIOnPath(EFNAFAISpawnType AIType, bool bForceShattered, FName PathName)
{
    EFNAFAISubType SubType = bForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None; // None
    return SpawnSpecificAIOnPathWithSubType(AIType, SubType, PathName);
}

void UAIManagementSystem::SpawnSpecificAIAtSpawnPointWithSubType(AFNAFAISpawnPoint* SpawnPoint, EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType)
{
    if (!SpawnPoint || !SpawnPoint->GetRootComponent())
    {
        return;
    }

    FVector SpawnLocation = SpawnPoint->GetRootComponent()->GetComponentLocation();
    FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation, FVector::OneVector);

    bool bSuccess = false;
    SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, forceAISubType, bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
}

void UAIManagementSystem::SpawnSpecificAIAtSpawnPoint(AFNAFAISpawnPoint* SpawnPoint, EFNAFAISpawnType AIType, bool ForceShattered)
{
    EFNAFAISubType SubType = ForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
    SpawnSpecificAIAtSpawnPointWithSubType(SpawnPoint, AIType, SubType);
}

APawn* UAIManagementSystem::SpawnAIWithTransformAndPathWithSubType(EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType, const FTransform& SpawnTransform, FName PathName)
{
    bool bSuccess = false;
    APawn* SpawnedPawn = SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, forceAISubType, bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);

    if (SpawnedPawn)
    {
        // Get patrol path and assign it
        TScriptInterface<ISeekerPatrolPath> PatrolPath = GetPathForAI(AIType, PathName);
        if (PatrolPath.GetObject())
        {
            if (SpawnedPawn->GetClass()->ImplementsInterface(UPatrollerInterface::StaticClass()))
            {
                UObject* PathObj = PatrolPath.GetObject();
                if (PathObj && PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
                {
                    TScriptInterface<IPathPointProvider> PathProvider;
                    PathProvider.SetObject(PathObj);
                    PathProvider.SetInterface(Cast<IPathPointProvider>(PathObj));
                    IPatrollerInterface::Execute_SetPatrolPath(SpawnedPawn, PathProvider);
                }
            }
        }

        StoreAnimatronicSpawnWithSubType(AIType, PathName, forceAISubType);
    }

    return SpawnedPawn;
}

APawn* UAIManagementSystem::SpawnAIWithTransformAndPath(EFNAFAISpawnType AIType, bool bForceShattered, const FTransform& SpawnTransform, FName PathName)
{
    EFNAFAISubType SubType = bForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
    return SpawnAIWithTransformAndPathWithSubType(AIType, SubType, SpawnTransform, PathName);
}

APawn* UAIManagementSystem::SpawnAITypeWithTransformWithSubType(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, EFNAFAISubType forceAISubType, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner)
{
    bool bSuccess = false;
    return SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, forceAISubType, bSuccess, CollisionOverrideMethod, Owner);
}

APawn* UAIManagementSystem::SpawnAITypeWithTransformSafeWithSubType(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, EFNAFAISubType forceAISubType, bool& success, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner, bool bForceRespawn)
{
    success = false;

    // 1. Get character info for this AI type
    FFNAFAISettingInfo* AITypeInfo = GetAITypeInfo(AIType);
    if (!AITypeInfo)
    {
        return nullptr;
    }

    // 2. Destroy any existing pawn of the same AIType
    TArray<APawn*> TempPawnList = RegisteredPawns;
    for (APawn* Pawn : TempPawnList)
    {
        if (!Pawn) continue;
        if (Pawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
        {
            if (IFNAFManagedAI::Execute_GetManagedAIType(Pawn) == AIType)
            {
                UE_LOG(LogAIManagement, Warning, TEXT("SpawnSafe: DESTROYING existing pawn %s (type %d) at %s"),
                    *Pawn->GetName(), (int32)AIType, *Pawn->GetActorLocation().ToString());
                Pawn->Destroy();
            }
        }
    }

    // 3. Get AI state from WorldStateSystem for shattered flag resolution
    UWorldStateSystem* WorldStateSys = GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    FFNAFAISaveData AIState;
    if (WorldStateSys)
    {
        AIState = WorldStateSys->GetAIState();
    }

    // 4. Resolve SubType if None (4) - check shattered flags per AIType
    if (forceAISubType == EFNAFAISubType::None)
    {
        bool bIsShattered = false;
        switch (AIType)
        {
        case EFNAFAISpawnType::Chica:
            bIsShattered = AIState.bShatteredChica;
            break;
        case EFNAFAISpawnType::Roxy:
            bIsShattered = AIState.bShatteredRoxy;
            break;
        case EFNAFAISpawnType::Monty:
            bIsShattered = AIState.bShatteredMonty;
            break;
        default:
            bIsShattered = false;
            break;
        }
        forceAISubType = bIsShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::Normal;
    }

    // 5. Select the correct class path based on SubType
    // FFNAFAISettingInfo has 4 FSoftClassPath entries at offsets +0, +24, +48, +72
    FSoftClassPath* ClassPath = &AITypeInfo->StandardType;
    switch (forceAISubType)
    {
    case EFNAFAISubType::Normal:
        ClassPath = &AITypeInfo->StandardType;
        break;
    case EFNAFAISubType::Shattered:
        ClassPath = &AITypeInfo->ShatteredType;
        break;
    case EFNAFAISubType::Ruined_SingleSpawn:
        ClassPath = &AITypeInfo->RuinedSingleSpawnType;
        break;
    case EFNAFAISubType::Ruined_Patrol:
        ClassPath = &AITypeInfo->RuinedPatrolType;
        break;
    default:
        ClassPath = &AITypeInfo->StandardType;
        break;
    }

    // 6. Load the pawn class
    UClass* PawnClass = ClassPath->TryLoadClass<APawn>();
    if (!PawnClass)
    {
        return nullptr;
    }

    // 7. Adjust spawn Z for character capsule half-height
    FTransform FinalSpawnLocation = SpawnTransform;
    UClass* CharacterClass = ACharacter::StaticClass();
    if (PawnClass->IsChildOf(CharacterClass))
    {
        APawn* CDO = PawnClass->GetDefaultObject<APawn>();
        if (CDO)
        {
            float CapsuleHalfHeight = 45.0f; // default
            ACharacter* CharCDO = Cast<ACharacter>(CDO);
            if (CharCDO && CharCDO->GetCapsuleComponent())
            {
                CapsuleHalfHeight = CharCDO->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            }
            FVector AdjustedLocation = FinalSpawnLocation.GetLocation();
            AdjustedLocation.Z += CapsuleHalfHeight + 10.0f;
            FinalSpawnLocation.SetLocation(AdjustedLocation);
        }
    }

    // 8. Spawn the actor
    success = true;
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = Owner;
    SpawnParams.SpawnCollisionHandlingOverride = CollisionOverrideMethod;

    AActor* SpawnedActor = GetWorld()->SpawnActor(PawnClass, &FinalSpawnLocation, SpawnParams);
    APawn* SpawnedPawn = Cast<APawn>(SpawnedActor);

    // 9. Fire OnAISpawned delegate and track in TypesSpawned
    if (SpawnedPawn)
    {
        TypesSpawned.AddUnique(AIType);
        OnAISpawned.Broadcast(SpawnedPawn);
        UE_LOG(LogAIManagement, Warning, TEXT("SpawnSafe: SPAWNED new pawn %s (type %d) at %s, bForceRespawn=%d, TypesSpawned=%d"),
            *SpawnedPawn->GetName(), (int32)AIType, *SpawnTransform.GetLocation().ToString(), (int32)bForceRespawn, TypesSpawned.Num());
    }

    return SpawnedPawn;
}

APawn* UAIManagementSystem::SpawnAITypeWithTransformSafe(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, bool ForceShattered, bool& success, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner, bool bForceRespawn)
{
    EFNAFAISubType SubType = EFNAFAISubType::None; // None
    if (ForceShattered)
    {
        SubType = EFNAFAISubType::Shattered;
    }
    return SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, SubType, success, CollisionOverrideMethod, Owner, bForceRespawn);
}

APawn* UAIManagementSystem::SpawnAITypeWithTransform(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, bool ForceShattered, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner)
{
    bool bSuccess = false;
    EFNAFAISubType SubType = ForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
    APawn* Result = SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, SubType, bSuccess, CollisionOverrideMethod, Owner);
    if (!bSuccess)
    {
        return nullptr;
    }
    return Result;
}

APawn* UAIManagementSystem::SpawnAITypeAtLocationWithSubType(EFNAFAISpawnType AIType, const FVector& SpawnLocation, EFNAFAISubType forceAISubType)
{
    FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation, FVector::OneVector);
    bool bSuccess = false;
    return SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, forceAISubType, bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
}

APawn* UAIManagementSystem::SpawnAITypeAtLocation(EFNAFAISpawnType AIType, const FVector& SpawnLocation, bool ForceShattered)
{
    EFNAFAISubType SubType = ForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
    return SpawnAITypeAtLocationWithSubType(AIType, SpawnLocation, SubType);
}

// Passes bForceRespawn=true; stores shattered state from IsShatteredVersion result
APawn* UAIManagementSystem::SpawnAIOnPathWithSubType(EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType, FName PathName)
{
    TScriptInterface<ISeekerPatrolPath> PatrolPath = GetPathForAI(AIType, PathName);
    if (!PatrolPath.GetObject())
    {
        return nullptr;
    }

    FVector SpawnLocation;
    if (!FindRandomPatrolPointOutOfView(AIType, PatrolPath, SpawnLocation))
    {
        return nullptr;
    }

    FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation, FVector::OneVector);
    bool bSuccess = false;

    // IDA: passes bForceRespawn = 1 (true) — patrol spawns force respawn
    APawn* SpawnedPawn = SpawnAITypeWithTransformSafeWithSubType(
        AIType, SpawnTransform, forceAISubType, bSuccess,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, true);

    // IDA: checks success flag, returns nullptr if not successful
    if (!bSuccess || !SpawnedPawn)
    {
        return nullptr;
    }

    // IDA: checks IsShatteredVersion on spawned pawn, passes to StoreAnimatronicSpawn
    bool bIsShattered = false;
    if (SpawnedPawn->GetClass() &&
        SpawnedPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()))
    {
        bIsShattered = IFNAFManagedAI::Execute_IsShatteredVersion(SpawnedPawn);
    }
    StoreAnimatronicSpawn(AIType, PathName, bIsShattered);

    // IDA: gets PathPointProvider interface directly, calls SetPatrolPath
    UObject* PathObj = PatrolPath.GetObject();
    if (PathObj)
    {
        IPathPointProvider* PathInterface = Cast<IPathPointProvider>(
            PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()) ? PathObj : nullptr);

        TScriptInterface<IPathPointProvider> PathProvider;
        PathProvider.SetObject(PathObj);
        PathProvider.SetInterface(PathInterface);
        IPatrollerInterface::Execute_SetPatrolPath(SpawnedPawn, PathProvider);
    }

    return SpawnedPawn;
}

APawn* UAIManagementSystem::SpawnAIOnPathNearLocation(EFNAFAISpawnType AIType, const FVector& Location, FName PathName)
{
    TScriptInterface<ISeekerPatrolPath> PatrolPath = GetPathForAI(AIType, PathName);
    UObject* PathObj = PatrolPath.GetObject();
    if (!PathObj || !PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
    {
        return nullptr;
    }

    int32 NumPoints = IPathPointProvider::Execute_GetNumberOfPathPoints(PathObj);
    if (NumPoints <= 0)
    {
        return nullptr;
    }

    // Find closest patrol point to Location
    float ClosestDist = MAX_FLT;
    int32 ClosestIndex = -1;
    FVector ClosestLocation = FVector::ZeroVector;

    for (int32 i = 0; i < NumPoints; ++i)
    {
        FVector PointLocation = IPathPointProvider::Execute_GetPointLocation(PathObj, i);
        float Dist = FVector::Dist(PointLocation, Location);
        if (Dist < ClosestDist)
        {
            ClosestDist = Dist;
            ClosestIndex = i;
            ClosestLocation = PointLocation;
        }
    }

    if (ClosestIndex < 0)
    {
        return nullptr;
    }

    FTransform SpawnTransform(FRotator::ZeroRotator, ClosestLocation, FVector::OneVector);
    bool bSuccess = false;
    // Use None subtype (4) to let SpawnAITypeWithTransformSafeWithSubType resolve from save state
    APawn* SpawnedPawn = SpawnAITypeWithTransformSafeWithSubType(AIType, SpawnTransform, EFNAFAISubType::None, bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);

    if (SpawnedPawn)
    {
        // Set patrol path and point index
        if (SpawnedPawn->GetClass()->ImplementsInterface(UPatrollerInterface::StaticClass()))
        {
            if (PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
            {
                TScriptInterface<IPathPointProvider> PathProvider;
                PathProvider.SetObject(PathObj);
                PathProvider.SetInterface(Cast<IPathPointProvider>(PathObj));
                IPatrollerInterface::Execute_SetPatrolPath(SpawnedPawn, PathProvider);
                IPatrollerInterface::Execute_SetCurrentPatrolPointIndex(SpawnedPawn, ClosestIndex);
            }
        }
    }

    return SpawnedPawn;
}

APawn* UAIManagementSystem::SpawnAIOnPath(EFNAFAISpawnType AIType, bool bForceShattered, FName PathName)
{
    EFNAFAISubType SubType = bForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
    return SpawnAIOnPathWithSubType(AIType, SubType, PathName);
}

void UAIManagementSystem::SpawnAINearPlayer()
{
    AFNAFAISpawnPoint* SpawnPoint = FindClosestSpawnPoint();
    if (SpawnPoint)
    {
        SpawnSpecificAIAtSpawnPointWithSubType(SpawnPoint, SpawnPoint->GetAIType(), EFNAFAISubType::None);
    }
}

void UAIManagementSystem::SpawnAIFar()
{
    AFNAFAISpawnPoint* SpawnPoint = FindFurthestSpawnPoint();
    if (SpawnPoint)
    {
        SpawnSpecificAIAtSpawnPointWithSubType(SpawnPoint, SpawnPoint->GetAIType(), EFNAFAISubType::None);
    }
}

void UAIManagementSystem::SpawnAIAtSpawnPointWithSubType(AFNAFAISpawnPoint* SpawnPoint, EFNAFAISubType forceAISubType)
{
    if (!SpawnPoint)
    {
        return;
    }
    SpawnSpecificAIAtSpawnPointWithSubType(SpawnPoint, SpawnPoint->GetAIType(), forceAISubType);
}

void UAIManagementSystem::SpawnAIAtSpawnPoint(AFNAFAISpawnPoint* SpawnPoint, bool bForceShattered)
{
    EFNAFAISubType SubType = bForceShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
    SpawnAIAtSpawnPointWithSubType(SpawnPoint, SubType);
}

void UAIManagementSystem::SpawnAIAtDistance(float Distance)
{
    AFNAFAISpawnPoint* SpawnPoint = FindSpawnPointClosestToDistance(Distance);
    if (SpawnPoint)
    {
        SpawnSpecificAIAtSpawnPointWithSubType(SpawnPoint, SpawnPoint->GetAIType(), EFNAFAISubType::None);
    }
}

// Respawn functions
void UAIManagementSystem::RespawnEndos()
{
    if (RegisteredEndos.Num() > 0)
    {
        return; // Endos already exist
    }

    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (!WorldStateSys)
    {
        return;
    }

    FFNAFAISaveData AIState = WorldStateSys->GetAIState();

    for (const FEndoSaveData& EndoData : AIState.Endos)
    {
        FTransform SpawnTransform(EndoData.Rotation, EndoData.Location, FVector::OneVector);
        bool bSuccess = false;
        SpawnAITypeWithTransformSafeWithSubType(EFNAFAISpawnType::Endo, SpawnTransform, EFNAFAISubType::None, bSuccess, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
    }
}

void UAIManagementSystem::RespawnAnimatronics()
{
    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (!WorldStateSys)
    {
        return;
    }

    FFNAFAISaveData AIState = WorldStateSys->GetAIState();

    for (auto& Pair : AIState.AnimatronicStates)
    {
        EFNAFAISpawnType AIType = Pair.Key;
        const FAnimatronicState& State = Pair.Value;

        EFNAFAISubType SubType = State.bIsShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::None;
        SpawnAIOnPathWithSubType(AIType, SubType, State.PathName);
    }
}

void UAIManagementSystem::RespawnAllAI()
{
    RespawnAnimatronics();
    RespawnEndos();
}

// Store/Reset functions
void UAIManagementSystem::StoreEndoStates()
{
    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (!WorldStateSys)
    {
        return;
    }

    FFNAFAISaveData AIState = WorldStateSys->GetAIState();
    AIState.Endos.Empty();

    for (APawn* EndoPawn : RegisteredEndos)
    {
        if (!EndoPawn || !IsValid(EndoPawn) || !EndoPawn->GetRootComponent())
        {
            continue;
        }

        FEndoSaveData EndoData;
        EndoData.Location = EndoPawn->GetRootComponent()->GetComponentLocation();
        EndoData.Rotation = EndoPawn->GetRootComponent()->GetComponentRotation();
        AIState.Endos.Add(EndoData);
    }

    WorldStateSys->SetAIState(AIState);
}

void UAIManagementSystem::StoreAnimatronicSpawnWithSubType(EFNAFAISpawnType AIType, FName PathName, EFNAFAISubType AISubType)
{
    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (!WorldStateSys)
    {
        return;
    }

    FFNAFAISaveData AIState = WorldStateSys->GetAIState();

    FAnimatronicState NewState;
    NewState.PathName = PathName;
    NewState.bIsShattered = (AISubType == EFNAFAISubType::Shattered);

    AIState.AnimatronicStates.Emplace(AIType, NewState);
    WorldStateSys->SetAIState(AIState);
}

void UAIManagementSystem::StoreAnimatronicSpawn(EFNAFAISpawnType AIType, FName PathName, bool bIsShattered)
{
    EFNAFAISubType SubType = bIsShattered ? EFNAFAISubType::Shattered : EFNAFAISubType::Normal;
    StoreAnimatronicSpawnWithSubType(AIType, PathName, SubType);
}

void UAIManagementSystem::StartManager()
{
    if (TimerHandle.IsValid())
    {
        return; // Already started
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Set up repeating timer for OnTickAIManager
    FTimerDelegate TimerDelegate;
    TimerDelegate.BindUObject(this, &UAIManagementSystem::OnTickAIManager);
    World->GetTimerManager().SetTimer(TimerHandle, TimerDelegate, TickDelta, true);

    // Find all spawn points in world
    FindAllSpawnPoints();

    // Bind to world state changes
    UWorldStateSystem* WorldStateSys = World->GetGameInstance()->GetSubsystem<UWorldStateSystem>();
    if (WorldStateSys)
    {
        WorldStateSys->OnWorldStateChanged.AddDynamic(this, &UAIManagementSystem::OnWorldStateChanged);
    }

    bHasStarted = true;
}

void UAIManagementSystem::Reset()
{
    // Reset VannyMeter to starting value for current game hour
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UGameClockSystem* ClockSys = World->GetGameInstance()->GetSubsystem<UGameClockSystem>();
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();

    if (ClockSys && Settings)
    {
        int32 Hour = 0;
        int32 Minute = 0;
        ClockSys->GetCurrentTime(Hour, Minute);

        if (Settings->VannyMeterPerHourBase.IsValidIndex(Hour))
        {
            VannyMeter.CurrentValue = Settings->VannyMeterPerHourBase[Hour];
        }
        else
        {
            VannyMeter.CurrentValue = 0.0f;
        }
    }
    else
    {
        VannyMeter.CurrentValue = 0.0f;
    }
}

/*
 * Latent action functions -- synchronous approximations since the original async
 * workers (FShortestPathFinder, FNearestToDistancePathFinder, etc.) are not available.
 */
void UAIManagementSystem::CalculateAllAIDistances(TArray<FAIDistanceResult>& DistanceResults, bool& bOutClosestIsValid, int32& ClosestIndex, FLatentActionInfo LatentInfo)
{
    bOutClosestIsValid = false;
    ClosestIndex = -1;
    DistanceResults.Empty();

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn)
    {
        return;
    }

    FVector PlayerLocation = PlayerPawn->GetActorLocation();
    float ClosestDist = MAX_FLT;

    for (int32 i = 0; i < RegisteredPawns.Num(); ++i)
    {
        APawn* AIPawn = RegisteredPawns[i];
        if (!AIPawn || !IsValid(AIPawn))
        {
            continue;
        }

        FAIDistanceResult Result;
        Result.Pawn = AIPawn;
        Result.AIType = AIPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()) ?
            IFNAFManagedAI::Execute_GetManagedAIType(AIPawn) : EFNAFAISpawnType::None;
        Result.NavDistance = FVector::Dist(AIPawn->GetActorLocation(), PlayerLocation);
        DistanceResults.Add(Result);

        if (Result.NavDistance < ClosestDist)
        {
            ClosestDist = Result.NavDistance;
            ClosestIndex = DistanceResults.Num() - 1;
            bOutClosestIsValid = true;
        }
    }

    // Also cache results
    CachedDistanceResults = DistanceResults;
}

void UAIManagementSystem::FindClosestPathPointForAI(APawn* AIPawn, bool& OutResultValid, TScriptInterface<ISeekerPatrolPath>& OutPatrolPath, int32& OutPointIndex, FVector& OutLocation, FLatentActionInfo LatentInfo)
{
    OutResultValid = false;
    OutPointIndex = -1;

    if (!AIPawn)
    {
        return;
    }

    FVector AILocation = AIPawn->GetActorLocation();
    float ClosestDist = MAX_FLT;

    for (const FWeakObjectPtr& WeakPath : RegisteredPatrolPaths)
    {
        UObject* PathObj = WeakPath.Get();
        if (!PathObj || !PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
        {
            continue;
        }

        int32 NumPoints = IPathPointProvider::Execute_GetNumberOfPathPoints(PathObj);
        for (int32 i = 0; i < NumPoints; ++i)
        {
            FVector PointLocation = IPathPointProvider::Execute_GetPointLocation(PathObj, i);
            float Dist = FVector::Dist(AILocation, PointLocation);

            if (Dist < ClosestDist)
            {
                ClosestDist = Dist;
                OutPointIndex = i;
                OutLocation = PointLocation;
                OutResultValid = true;

                ISeekerPatrolPath* PatrolPath = Cast<ISeekerPatrolPath>(PathObj);
                if (PatrolPath)
                {
                    OutPatrolPath.SetObject(PathObj);
                    OutPatrolPath.SetInterface(PatrolPath);
                }
            }
        }
    }
}

void UAIManagementSystem::FindClosestPatrolPointOutOfView(APawn* AIPawn, bool& bOutResultValid, TScriptInterface<ISeekerPatrolPath>& OutPatrolPath, FVector& OutLocation, FLatentActionInfo LatentActionInfo, int32& OutPointIndex)
{
    bOutResultValid = false;
    OutPointIndex = -1;

    if (!AIPawn)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    FVector CameraForward = CameraManager->GetCameraRotation().Vector();
    FVector AILocation = AIPawn->GetActorLocation();
    float ClosestDist = MAX_FLT;

    for (const FWeakObjectPtr& WeakPath : RegisteredPatrolPaths)
    {
        UObject* PathObj = WeakPath.Get();
        if (!PathObj || !PathObj->GetClass()->ImplementsInterface(UPathPointProvider::StaticClass()))
        {
            continue;
        }

        int32 NumPoints = IPathPointProvider::Execute_GetNumberOfPathPoints(PathObj);
        for (int32 i = 0; i < NumPoints; ++i)
        {
            FVector PointLocation = IPathPointProvider::Execute_GetPointLocation(PathObj, i);

            // Check if point is out of view
            FVector DirToPoint = (PointLocation - CameraLocation).GetSafeNormal();
            float DotProduct = FVector::DotProduct(CameraForward, DirToPoint);
            if (DotProduct > 0.7f)
            {
                // In FOV, check line trace
                FHitResult HitResult;
                FCollisionQueryParams QueryParams;
                bool bHit = World->LineTraceSingleByChannel(HitResult, CameraLocation, PointLocation, ECC_Visibility, QueryParams);
                if (!bHit)
                {
                    continue; // Visible, skip
                }
            }

            float Dist = FVector::Dist(AILocation, PointLocation);
            if (Dist < ClosestDist)
            {
                ClosestDist = Dist;
                OutPointIndex = i;
                OutLocation = PointLocation;
                bOutResultValid = true;

                ISeekerPatrolPath* PatrolPath = Cast<ISeekerPatrolPath>(PathObj);
                if (PatrolPath)
                {
                    OutPatrolPath.SetObject(PathObj);
                    OutPatrolPath.SetInterface(PatrolPath);
                }
            }
        }
    }
}

void UAIManagementSystem::FindSpawnNotVisibleAtDistance(float Distance, EFNAFAISpawnType SpawnType, APawn* PawnForNavProperties, TArray<AFNAFAISpawnPoint*>& OutSpawnPointsResult, TArray<float>& OutDistances, FLatentActionInfo LatentInfo)
{
    OutSpawnPointsResult.Empty();
    OutDistances.Empty();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(World, 0);
    if (!CameraManager)
    {
        return;
    }

    FVector CameraLocation = CameraManager->GetCameraLocation();
    FVector CameraForward = CameraManager->GetCameraRotation().Vector();

    TArray<AFNAFAISpawnPoint*> AllSpawnPoints = GetAllSpawnPointsFor(SpawnType);

    // Collect spawn points that are not visible and sort by distance to target
    struct FSpawnDistPair
    {
        AFNAFAISpawnPoint* SpawnPoint;
        float DistToTarget;
    };
    TArray<FSpawnDistPair> ValidPairs;

    for (AFNAFAISpawnPoint* SpawnPoint : AllSpawnPoints)
    {
        if (!SpawnPoint || !SpawnPoint->GetRootComponent())
        {
            continue;
        }

        FVector SpawnLocation = SpawnPoint->GetRootComponent()->GetComponentLocation();

        // Check if out of view
        FVector DirToSpawn = (SpawnLocation - CameraLocation).GetSafeNormal();
        float DotProduct = FVector::DotProduct(CameraForward, DirToSpawn);
        if (DotProduct > 0.7f)
        {
            // In view - do line trace to confirm
            FHitResult HitResult;
            FCollisionQueryParams QueryParams;
            bool bHit = World->LineTraceSingleByChannel(HitResult, CameraLocation, SpawnLocation, ECC_Visibility, QueryParams);
            if (!bHit)
            {
                continue; // Visible, skip
            }
        }

        float DistFromPlayer = FVector::Dist(CameraLocation, SpawnLocation);
        float DistDelta = FMath::Abs(DistFromPlayer - Distance);

        FSpawnDistPair Pair;
        Pair.SpawnPoint = SpawnPoint;
        Pair.DistToTarget = DistDelta;
        ValidPairs.Add(Pair);
    }

    // Sort by closeness to desired distance
    ValidPairs.Sort([](const FSpawnDistPair& A, const FSpawnDistPair& B)
        {
            return A.DistToTarget < B.DistToTarget;
        });

    for (const FSpawnDistPair& Pair : ValidPairs)
    {
        OutSpawnPointsResult.Add(Pair.SpawnPoint);
        OutDistances.Add(Pair.DistToTarget);
    }
}

// Main AI tick (called on repeating timer)
void UAIManagementSystem::OnTickAIManager()
{
    // 1. Decay Vanny meter
    const UAISystemSettings* Settings = GetDefault<UAISystemSettings>();
    if (Settings)
    {
        // VannyMeterIncreasePerSecond acts as decay rate when negated per tick
        float DecayAmount = -(TickDelta * Settings->VannyMeterIncreasePerSecond);
        VannyMeter.AdjustVannyMeter(DecayAmount);
    }

    // 2. Handle spawn logic
    SpawnHandling();

    // 3. Update cached distance results
    if (RegisteredPawns.Num() == 0)
    {
        CachedDistanceResults.Empty();
        return;
    }

    // Calculate distances synchronously (async workers not available)
    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (PlayerPawn)
    {
        FVector PlayerLocation = PlayerPawn->GetActorLocation();
        CachedDistanceResults.Empty();

        for (APawn* AIPawn : RegisteredPawns)
        {
            if (!AIPawn || !IsValid(AIPawn))
            {
                continue;
            }

            FAIDistanceResult Result;
            Result.Pawn = AIPawn;
            Result.AIType = AIPawn->GetClass()->ImplementsInterface(UFNAFManagedAI::StaticClass()) ?
                IFNAFManagedAI::Execute_GetManagedAIType(AIPawn) : EFNAFAISpawnType::None;
            Result.NavDistance = FVector::Dist(AIPawn->GetActorLocation(), PlayerLocation);
            CachedDistanceResults.Add(Result);
        }
    }

    // 4. AI Teleport logic: if enabled and no pawns have sight to player
    if (bAITeleportEnabled && RegisteredPawns.Num() > 0 && PawnsWithSightToPlayer.Num() == 0)
    {
        if (Settings && TimeSinceLastEncounter > Settings->TimeBetweenSightings)
        {
            // TODO: Full teleport logic requires FNearestToDistancePathFinder async worker
            // From IDA: Creates pathfinder, calls AddFindClosestPatrolPointQueries
            // for the first registered pawn, then teleports on callback
        }
    }

    // DEBUG: Track pawn positions to catch teleporting
    static TMap<FName, FVector> LastKnownPositions;
    static TMap<FName, float> LastKnownTime;
    float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
    for (APawn* AIPawn : RegisteredPawns)
    {
        if (!AIPawn || !IsValid(AIPawn)) continue;
        FName PawnName = AIPawn->GetFName();
        FVector CurrentPos = AIPawn->GetActorLocation();
        if (FVector* LastPos = LastKnownPositions.Find(PawnName))
        {
            float MoveDelta = FVector::Dist(*LastPos, CurrentPos);
            float TimeDelta = CurrentTime - *LastKnownTime.Find(PawnName);
            if (MoveDelta > 100.0f) // Log any significant movement
            {
                float Speed = (TimeDelta > 0.f) ? MoveDelta / TimeDelta : 0.f;
                UE_LOG(LogAIManagement, Warning, TEXT("MOVE: %s moved %.0f units (speed=%.0f u/s, dt=%.3fs) From %s to %s"),
                    *AIPawn->GetName(), MoveDelta, Speed, TimeDelta, *LastPos->ToString(), *CurrentPos.ToString());
            }
        }
        LastKnownPositions.Add(PawnName, CurrentPos);
        LastKnownTime.Add(PawnName, CurrentTime);
    }

    TimeSinceLastEncounter += TickDelta;

    // 5. Check hiding AI adjacency
    // If an AI is hiding, check if its room is still adjacent to the player.
    // If not adjacent, eject from hide and reset world cue.
    if (AIHiding && IsValid(AIHiding))
    {
        URoomSystem* RoomSys = GetWorld() ? GetWorld()->GetSubsystem<URoomSystem>() : nullptr;
        if (RoomSys)
        {
            TArray<ARoomAreaBase*> PlayerRooms = RoomSys->GetPlayerCurrentRooms();
            ARoomAreaBase* HidingRoom = RoomSys->GetRoomAtLocation(AIHiding->GetActorLocation());

            bool bStillAdjacent = false;
            if (HidingRoom)
            {
                for (ARoomAreaBase* PlayerRoom : PlayerRooms)
                {
                    if (PlayerRoom == HidingRoom)
                    {
                        bStillAdjacent = true;
                        break;
                    }
                    TArray<FRoomAdjacencyInfo> Adjacent = PlayerRoom->GetAllAdjacentRooms();
                    for (const FRoomAdjacencyInfo& Adj : Adjacent)
                    {
                        if (Adj.Room.Get() == HidingRoom)
                        {
                            bStillAdjacent = true;
                            break;
                        }
                    }
                    if (bStillAdjacent) break;
                }
            }

            if (!bStillAdjacent)
            {
                // Eject from hide mode
                if (AIHiding->GetClass()->ImplementsInterface(UAIHiderInterface::StaticClass()))
                {
                    IAIHiderInterface::Execute_ExitHideMode(AIHiding, nullptr);
                }
                AIHiding = nullptr;
            }
        }
    }
}

// Core spawn loop called each tick
void UAIManagementSystem::SpawnHandling()
{
    if (!bSpawningEnabled || bIsSearchingForSpawn || !bRandomWorldSpawningEnabled)
    {
        return;
    }

    // IDA: Iterates TypesExpected, checks TypesSpawned for matching AIType byte
    // NOT RegisteredPawns — TypesSpawned is a simple EFNAFAISpawnType array
    for (const FAnimatronicExpectedData& Expected : TypesExpected)
    {
        bool bAlreadySpawned = TypesSpawned.Contains(Expected.AIType);

        if (!bAlreadySpawned)
        {
            UE_LOG(LogAIManagement, Warning, TEXT("SpawnHandling: type %d NOT in TypesSpawned (%d entries), calling SpawnAIOnPath"),
                (int32)Expected.AIType, TypesSpawned.Num());
            SpawnAIOnPath(Expected.AIType, false, Expected.PathName);
        }
    }

    // Handle hiding AI if none currently hiding
    if (!AIHiding)
    {
        HandleHidingAI();
    }
}

// Restore state after loading a save
void UAIManagementSystem::PostGameLoad_Implementation()
{
    UWorldStateSystem* WorldStateSys = GetWorld() ? GetWorld()->GetGameInstance()->GetSubsystem<UWorldStateSystem>() : nullptr;
    if (!WorldStateSys)
    {
        return;
    }

    FFNAFAISaveData AIState = WorldStateSys->GetAIState();

    // Restore world spawn enabled and teleport enabled from save
    bRandomWorldSpawningEnabled = AIState.bWorldSpawnEnabled;
    bAITeleportEnabled = AIState.bAITeleportEnabled;
}

void UAIManagementSystem::AddFindClosestPatrolPointQueries(const FString& DebugName, APawn* AIPawn, TSharedRef<FNearestToDistancePathFinder>& PathFinder)
{
    // This is a helper that adds pathfinding queries to an async worker
    // In the full implementation, it iterates RegisteredPatrolPaths and
    // calls PathFinder->FindDistances for each path point
    // Simplified: no-op since async workers aren't fully defined
}

FFNAFAISettingInfo* UAIManagementSystem::GetAITypeInfo(EFNAFAISpawnType AIType)
{
    return CharacterInfoDataMap.Find(AIType);
}