#pragma once
#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/LatentActionManager.h"
#include "Subsystems/WorldSubsystem.h"
#include "AIDistanceResult.h"
#include "AIRoomEntryInfo.h"
#include "AnimatronicExpectedData.h"
#include "AnimatronicTypeData.h"
#include "EFNAFAISpawnType.h"
#include "EFNAFAISubType.h"
#include "EFNAFGameState.h"
#include "FNAFAISettingInfo.h"
#include "OnAISpawnedDelegate.h"
#include "SaveHandlerInterface.h"
#include "Templates/SubclassOf.h"
#include "AIManagementSystem.generated.h"

class AActor;
class AFNAFAISpawnPoint;
class APawn;
class ARoomAreaBase;
class ISeekerPatrolPath;
class USeekerPatrolPath;
class UAIEnvironmentQueryInfo;

// Internal struct for tracking Vanny encounter meter (from IDA)
struct FVannyMeter
{
    float CurrentValue = 0.0f;
    FScriptDelegate OnVannyMeterMaxed;
    int32 CurrentGameHour = 0;

    void AdjustVannyMeter(float Amount)
    {
        CurrentValue += Amount;
        if (CurrentValue < 0.0f)
        {
            CurrentValue = 0.0f;
        }
    }
};

// RoomEntryList uses the project-defined FAIRoomEntryInfo (APawn* AIPawn, ARoomAreaBase* RoomEntering)

// Wrapper to avoid UHT nested container error (TMap<K, TArray<V>>)
USTRUCT()
struct FRoomPawnList
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient)
    TArray<APawn*> Pawns;
};

UCLASS(Blueprintable)
class FNAF9_API UAIManagementSystem : public UWorldSubsystem, public ISaveHandlerInterface {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool EnableDebugCloak;

    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnAISpawned OnAISpawned;

private:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TMap<EFNAFAISpawnType, FFNAFAISettingInfo> CharacterInfoDataMap;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<UAIEnvironmentQueryInfo*> QueryData;

    UPROPERTY(Transient)
    TArray<APawn*> PawnsWithSightToPlayer;

    // From AISpottedPlayer: set to 0.0 on spotted
    float TimeSinceLastEncounter = 0.0f;

    // From AddExpectedAI/ClearExpectedAI: array of expected AI data
    TArray<FAnimatronicExpectedData> TypesExpected;

    // From AdjustVannyMeter, GetCurrentVannyMeterPercentage, etc.
    FVannyMeter VannyMeter;

    // From FindAllSpawnPoints, GetAllSpawnPoints: weak references to spawn points
    TArray<TWeakObjectPtr<AFNAFAISpawnPoint>> SpawnPoints;

    // From GetPawnForType, GetExistingPawn, GetAllAnimatronicPawns, etc.
    UPROPERTY(Transient)
    TArray<APawn*> RegisteredPawns;

    // From GetPathForAI, GetPathByNameForAI, FindClosestPathPointForAI, etc.
    TArray<FWeakObjectPtr> RegisteredPatrolPaths;

    // From GetRoomAIPawnIsIn, GetRoomDistancesToPlayer, DestroyAllAIInRoomsAtleast
    UPROPERTY(Transient)
    TMap<APawn*, ARoomAreaBase*> RoomOccupancy;

    // From AnyPawnInPlayerRoom, GetAIPawnInRoom
    UPROPERTY(Transient)
    TMap<ARoomAreaBase*, FRoomPawnList> RoomOccupancyByRoom;

    // From GetRoomAIPawnIsEntering, GetAIPawnsEnteringRoom
    TArray<FAIRoomEntryInfo> RoomEntryList;

    // From GetCachedDistanceFor, CalculateAllAIDistances
    TArray<FAIDistanceResult> CachedDistanceResults;

    // From ExitedHiding, HandleHidingAI
    UPROPERTY(Transient)
    APawn* AIHiding = nullptr;

    // From ExitedHiding, HandleHidingAI
    UPROPERTY(Transient)
    AActor* CurrentHideLocation = nullptr;

    // From GetAllSpawnPoints: staged spawn filtering
    bool bUseStagedSpawns = false;

    // From DoNotDespawnAIDuringMoonmanPhase
    bool DoNotDespawnAIDuringMoonmanTime = false;

    // From FindRandomPatrolPointOutOfView: minimum spawn distance
    // Constructor: AISpawnOnLoadProximity = 500.0f
    float AISpawnOnLoadProximity = 500.0f;

    // Constructor: *(_QWORD*)&bSpawningEnabled = 257 → packs two bools: bSpawningEnabled=1, bRandomWorldSpawningEnabled=1
    bool bSpawningEnabled = true;
    bool bRandomWorldSpawningEnabled = true;

    // From SetVanessaSpawnEnable
    bool bVanessaSpawnEnabled = false;

    // From SetAITeleportEnabled/IsAITeleportEnabled
    bool bAITeleportEnabled = true;

    // From constructor: timer tick rate = 1.0f
    float TickDelta = 1.0f;

public:
    // From StartManager: repeating timer handle
    // Accessed directly by GameMode::StartRepairGame (per IDA)
    FTimerHandle TimerHandle;

private:
    UPROPERTY(Transient)
    APawn* PawnTrackingPlayer = nullptr;

    // From OnTickAIManager: current room monitored for AI hiding adjacency
    UPROPERTY(Transient)
    ARoomAreaBase* RoomBeingTracked = nullptr;

    // From PostGameLoad_Implementation: path length cache (also used for shattered flags storage)
    TMap<APawn*, float> PawnPathLengths;

    // From OnTickAIManager: concurrent query tracking
    int32 ActivePawnQueryCount = 0;

    // From SpawnHandling: tracks which AI types have been spawned
    TArray<EFNAFAISpawnType> TypesSpawned;

    // From RegisterAI/OnPawnEndPlay: endo-type pawns tracked separately
    UPROPERTY(Transient)
    TArray<APawn*> RegisteredEndos;

    // From SpawnHandling: prevents concurrent spawn searches
    bool bIsSearchingForSpawn = false;

    // From StartManager: manager initialization flag
    bool bHasStarted = false;

    // From OnAICharacterInfoLoaded: async load completion flag
    bool bCharacterInfoLoaded = false;

    // Private helper methods discovered from IDA
    FFNAFAISettingInfo* GetAITypeInfo(EFNAFAISpawnType AIType);
    void FindAllSpawnPoints();
    void HandleHidingAI();
    void SpawnHandling();
    void OnTickAIManager();
    UFUNCTION()
    void OnWorldStateChanged(EFNAFGameState NewState, EFNAFGameState OldState);
    UFUNCTION()
    void OnPawnEndPlay(AActor* DestroyedPawn, EEndPlayReason::Type EndPlayReason);
    void GetAllSpawnPointsFor(TArray<AFNAFAISpawnPoint*>& OutSpawnPoints, EFNAFAISpawnType AIType) const;
    void AddFindClosestPatrolPointQueries(const FString& DebugName, APawn* AIPawn, TSharedRef<struct FNearestToDistancePathFinder>& PathFinder);

    // From ISaveHandlerInterface
    virtual void PostGameLoad_Implementation() override;

public:
    UAIManagementSystem();

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    void AddSpawnPoint(AFNAFAISpawnPoint* SpawnPoint);
    void RemoveSpawnPoint(AFNAFAISpawnPoint* SpawnPoint);

    UFUNCTION(BlueprintCallable)
    void UnregisterSeekerPath(TScriptInterface<ISeekerPatrolPath> SeekerPatrolPath);
    UFUNCTION(BlueprintCallable)
    void UnRegisterAI(APawn* AIPawn);
    UFUNCTION(BlueprintCallable)
    void UnpauseManager();
    UFUNCTION(BlueprintCallable)
    void StoreEndoStates();
    UFUNCTION(BlueprintCallable)
    void StoreAnimatronicSpawnWithSubType(EFNAFAISpawnType AIType, FName PathName, EFNAFAISubType AISubType);
    UFUNCTION(BlueprintCallable)
    void StoreAnimatronicSpawn(EFNAFAISpawnType AIType, FName PathName, bool bIsShattered);
    UFUNCTION(BlueprintCallable)
    void StartManager();
    UFUNCTION(BlueprintCallable, meta = (Latent, LatentInfo = "LatentActionInfo"))
    void SpawnVannyOrVanessa(bool bSpawnVanny, bool& bOutSpawned, FLatentActionInfo LatentActionInfo);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnSpecificAIOnPathWithSubType(EFNAFAISpawnType AIType, EFNAFAISubType AISubType, FName PathName);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnSpecificAIOnPath(EFNAFAISpawnType AIType, bool bForceShattered, FName PathName);
    UFUNCTION(BlueprintCallable)
    void SpawnSpecificAIAtSpawnPointWithSubType(AFNAFAISpawnPoint* SpawnPoint, EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType);
    UFUNCTION(BlueprintCallable)
    void SpawnSpecificAIAtSpawnPoint(AFNAFAISpawnPoint* SpawnPoint, EFNAFAISpawnType AIType, bool ForceShattered);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAIWithTransformAndPathWithSubType(EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType, const FTransform& SpawnTransform, FName PathName);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAIWithTransformAndPath(EFNAFAISpawnType AIType, bool bForceShattered, const FTransform& SpawnTransform, FName PathName);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAITypeWithTransformWithSubType(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, EFNAFAISubType forceAISubType, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAITypeWithTransformSafeWithSubType(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, EFNAFAISubType forceAISubType, bool& success, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner, bool bForceRespawn = false);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAITypeWithTransformSafe(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, bool ForceShattered, bool& success, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner, bool bForceRespawn = false);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAITypeWithTransform(EFNAFAISpawnType AIType, const FTransform& SpawnTransform, bool ForceShattered, ESpawnActorCollisionHandlingMethod CollisionOverrideMethod, AActor* Owner);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAITypeAtLocationWithSubType(EFNAFAISpawnType AIType, const FVector& SpawnLocation, EFNAFAISubType forceAISubType);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAITypeAtLocation(EFNAFAISpawnType AIType, const FVector& SpawnLocation, bool ForceShattered);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAIOnPathWithSubType(EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType, FName PathName);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAIOnPathNearLocation(EFNAFAISpawnType AIType, const FVector& Location, FName PathName);
    UFUNCTION(BlueprintCallable)
    APawn* SpawnAIOnPath(EFNAFAISpawnType AIType, bool bForceShattered, FName PathName);
    UFUNCTION(BlueprintCallable)
    void SpawnAINearPlayer();
    UFUNCTION(BlueprintCallable)
    void SpawnAIFar();
    UFUNCTION(BlueprintCallable)
    void SpawnAIAtSpawnPointWithSubType(AFNAFAISpawnPoint* SpawnPoint, EFNAFAISubType forceAISubType);
    UFUNCTION(BlueprintCallable)
    void SpawnAIAtSpawnPoint(AFNAFAISpawnPoint* SpawnPoint, bool bForceShattered);
    UFUNCTION(BlueprintCallable)
    void SpawnAIAtDistance(float Distance);
    UFUNCTION(BlueprintCallable)
    void SetWorldSpawnEnabled(bool bEnable);
    UFUNCTION(BlueprintCallable)
    void SetVanessaSpawnEnable(bool bEnable);
    UFUNCTION(BlueprintCallable)
    void SetUseStagedSpawns(bool enable);
    UFUNCTION(BlueprintCallable)
    void SetSpawningEnabled(bool enable);
    UFUNCTION(BlueprintCallable)
    void SetExpectedAI(const TArray<FAnimatronicExpectedData>& AITypes);
    UFUNCTION(BlueprintCallable)
    void SetAllAIExpected();
    UFUNCTION(BlueprintCallable)
    void SetAITeleportEnabled(bool bEnable);
    UFUNCTION(BlueprintCallable)
    void SendVanessaAlert(TArray<EFNAFAISpawnType> TypesToAlert, int32 NumberToAlert);
    UFUNCTION(BlueprintCallable)
    void SendGeneralAlert(const FVector& AlertLocation, TArray<FAnimatronicTypeData> TypesToAlert, int32 NumberToAlert);

    /**
     * Spawns an AI near the player using EQS to find a not-visible position.
     * Called as fallback when SendGeneralAlert can't find an existing AI to alert.
     * IDA: standalone function SpawnAlertAI, called from 4 sites in SendGeneralAlert.
     *
     * Base game: uses /Game/Blueprints/AI/EnvQueries/EQNearPlayerNotVisible
     * DLC (Ruin): searches QueryData for matching type, falls back to DLC version
     */
    void SpawnAlertAI(const FVector& AlertLocation, const FAnimatronicTypeData& SpawnType);
    UFUNCTION(BlueprintCallable)
    void RespawnEndos();
    UFUNCTION(BlueprintCallable)
    void RespawnAnimatronics();
    UFUNCTION(BlueprintCallable)
    void RespawnAllAI();
    UFUNCTION(BlueprintCallable)
    void Reset();
    UFUNCTION(BlueprintCallable)
    void RemoveExpectedAI(EFNAFAISpawnType AIType);
    UFUNCTION(BlueprintCallable)
    void RemoveCharacterByType(EFNAFAISpawnType AIType);
    UFUNCTION(BlueprintCallable)
    void RemoveAllCharacters();
    UFUNCTION(BlueprintCallable)
    void RegisterSeekerPath(TScriptInterface<ISeekerPatrolPath> SeekerPatrolPath);
    UFUNCTION(BlueprintCallable)
    void RegisterAI(APawn* AIPawn);
    UFUNCTION(BlueprintCallable)
    void PawnExitedRoom(APawn* AIPawn, ARoomAreaBase* Room);
    UFUNCTION(BlueprintCallable)
    void PawnEnteringRoom(APawn* AIPawn, ARoomAreaBase* Room);
    UFUNCTION(BlueprintCallable)
    void PawnEnteredRoom(APawn* AIPawn, ARoomAreaBase* Room);
    UFUNCTION(BlueprintCallable)
    void PauseManager();

private:
    UFUNCTION(BlueprintCallable)
    void OnVannyPathsCollected();

public:
    UFUNCTION(BlueprintCallable)
    void OnAIFellOutOfWorld(APawn* AIPawn);

private:
    UFUNCTION(BlueprintCallable)
    void OnAIEnvQueryInfoLoaded(TArray<FAssetData> AssetDataList);
    UFUNCTION(BlueprintCallable)
    void OnAICharacterInfoLoaded(TArray<FAssetData> AssetDataList);

public:
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsWorldSpawnEnabled() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsUsingStagedSpawns() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsSpawningEnabled() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsRoomOccupied(ARoomAreaBase* Room) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsRoomBeingEntered(ARoomAreaBase* Room) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsAITeleportEnabled() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetTimeSinceLastEncounter() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TMap<APawn*, int32> GetRoomDistancesToPlayer() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    ARoomAreaBase* GetRoomAIPawnIsIn(APawn* AIPawn) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    ARoomAreaBase* GetRoomAIPawnIsEntering(APawn* AIPawn) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    APawn* GetPawnForType(EFNAFAISpawnType AIType) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TSubclassOf<APawn> GetPawnClassForTypeAndSubType(EFNAFAISpawnType AIType, EFNAFAISubType forceAISubType) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TSubclassOf<APawn> GetPawnClassForType(EFNAFAISpawnType AIType, bool bForceShattered) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TScriptInterface<ISeekerPatrolPath> GetPathForAI(EFNAFAISpawnType AIType, FName PathName) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TScriptInterface<ISeekerPatrolPath> GetPathByNameForAI(FName PathName) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<EFNAFAISpawnType> GetExistingPawnTypes() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    APawn* GetExistingPawn(EFNAFAISpawnType AIType, bool RequireShattered) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    void GetCurrentVannyMeterValuesAsPercentage(float& OutCurrentValuePercent, float& OutSoftMaxPercent) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    void GetCurrentVannyMeterValues(float& CurrentValue, float& Max, float& SoftMax) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    float GetCurrentVannyMeterPercentage() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<FAIDistanceResult> GetCachedDistances() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    void GetCachedDistanceFor(APawn* Pawn, bool& bOutResultValid, FAIDistanceResult& OutResult) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<AFNAFAISpawnPoint*> GetAllSpawnPointsFor(EFNAFAISpawnType AIType) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<AFNAFAISpawnPoint*> GetAllSpawnPoints() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<APawn*> GetAllRegisteredAI() const;

    const TArray<FWeakObjectPtr>& GetRegisteredPatrolPaths() const { return RegisteredPatrolPaths; }
    UFUNCTION(BlueprintCallable, BlueprintPure = false)
    void GetAllAnimatronicPawns(TArray<APawn*>& OutAnimatronicPawns) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<APawn*> GetAllAIInRoomAtMost(int32 numRooms) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<APawn*> GetAllAI() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<APawn*> GetAIPawnsWithSightToPlayer() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<APawn*> GetAIPawnsEnteringRoom(ARoomAreaBase* Room) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<APawn*> GetAIPawnInRoom(ARoomAreaBase* Room) const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    AFNAFAISpawnPoint* FindSpawnPointClosestToDistance(float Distance) const;
    UFUNCTION(BlueprintCallable, meta = (Latent, LatentInfo = "LatentInfo"))
    void FindSpawnNotVisibleAtDistance(float Distance, EFNAFAISpawnType SpawnType, APawn* PawnForNavProperties, TArray<AFNAFAISpawnPoint*>& OutSpawnPointsResult, TArray<float>& OutDistances, FLatentActionInfo LatentInfo);
    UFUNCTION(BlueprintCallable)
    bool FindRandomPatrolPointOutOfView(EFNAFAISpawnType AIType, TScriptInterface<ISeekerPatrolPath> PatrolPath, FVector& OutLocation);
    UFUNCTION(BlueprintCallable, BlueprintPure)
    AFNAFAISpawnPoint* FindFurthestSpawnPoint() const;
    UFUNCTION(BlueprintCallable, BlueprintPure)
    AFNAFAISpawnPoint* FindClosestSpawnPoint() const;
    UFUNCTION(BlueprintCallable, meta = (Latent, LatentInfo = "LatentActionInfo"))
    void FindClosestPatrolPointOutOfView(APawn* AIPawn, bool& bOutResultValid, TScriptInterface<ISeekerPatrolPath>& OutPatrolPath, FVector& OutLocation, FLatentActionInfo LatentActionInfo, int32& OutPointIndex);
    UFUNCTION(BlueprintCallable, meta = (Latent, LatentInfo = "LatentInfo"))
    void FindClosestPathPointForAI(APawn* AIPawn, bool& OutResultValid, TScriptInterface<ISeekerPatrolPath>& OutPatrolPath, int32& OutPointIndex, FVector& OutLocation, FLatentActionInfo LatentInfo);
    UFUNCTION(BlueprintCallable)
    void ExitedHiding(APawn* AIPawn);
    UFUNCTION(BlueprintCallable)
    void DoNotDespawnAIDuringMoonmanPhase(bool bDoNotDestroy);
    UFUNCTION(BlueprintCallable)
    void DestroyAllAINotVisible();
    UFUNCTION(BlueprintCallable)
    void DestroyAllAIInRoomsAtleast(int32 RoomDist);
    UFUNCTION(BlueprintCallable)
    void DestroyAllAI();
    UFUNCTION(BlueprintCallable)
    void ClearExpectedAI();
    UFUNCTION(BlueprintCallable, meta = (Latent, LatentInfo = "LatentInfo"))
    void CalculateAllAIDistances(TArray<FAIDistanceResult>& DistanceResults, bool& bOutClosestIsValid, int32& ClosestIndex, FLatentActionInfo LatentInfo);
    UFUNCTION(BlueprintCallable)
    void ApplySound(float Strength, const FVector& WorldLocation);
    UFUNCTION(BlueprintCallable)
    void ApplyCollect(const FVector& WorldLocation);
    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool AnyPawnInPlayerRoom() const;
    UFUNCTION(BlueprintCallable)
    void AISpottedPlayer(APawn* AIPawn);
    UFUNCTION(BlueprintCallable)
    void AILostSightOfPlayer(APawn* AIPawn);
    UFUNCTION(BlueprintCallable)
    void AdjustVannyMeter(float Amount);
    UFUNCTION(BlueprintCallable)
    void AddExpectedAI(FAnimatronicExpectedData AIType);
};