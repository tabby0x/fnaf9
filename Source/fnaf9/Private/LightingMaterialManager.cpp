#include "LightingMaterialManager.h"
#include "LightScenarioManager.h"
#include "OnLightScenarioChangeParamDelegate.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"

ALightingMaterialManager::ALightingMaterialManager()
{
    PrimaryActorTick.bCanEverTick = false;
}

/*
 * Collects unique streaming levels from soft mesh actor references, binds
 * OnLevelVisible to each level's OnLevelShown, applies initial materials,
 * and binds to LightScenarioManager's OnBeginScenarioChange.
 */
void ALightingMaterialManager::BeginPlay()
{
    AActor::BeginPlay();

    TSet<ULevelStreaming*> UsedLevels;

    for (int32 i = 0; i < MeshMaterials.Num(); ++i)
    {
        FLightMeshMaterialInfo& Info = MeshMaterials[i];
        for (int32 j = 0; j < Info.StaticMeshActor.Num(); ++j)
        {
            FString PackageName = Info.StaticMeshActor[j].ToSoftObjectPath().GetLongPackageName();
            FName PackageFName(*PackageName);
            ULevelStreaming* LevelStreaming = GetWorld()->GetLevelStreamingForPackageName(PackageFName);

            if (LevelStreaming && LevelStreaming->IsValidStreamingLevel())
            {
                UsedLevels.Add(LevelStreaming);
            }
        }
    }

    for (ULevelStreaming* Level : UsedLevels)
    {
        Level->OnLevelShown.AddDynamic(this, &ALightingMaterialManager::OnLevelVisible);
    }

    RefreshMaterials();

    ULightScenarioManager* ScenarioMgr = GetWorld()->GetSubsystem<ULightScenarioManager>();
    if (ScenarioMgr)
    {
        FOnLightScenarioChangeParam Delegate;
        Delegate.BindDynamic(this, &ALightingMaterialManager::OnBeginLightScenarioChange);
        ScenarioMgr->BindOnBeginScenarioChange(Delegate);
    }
}

void ALightingMaterialManager::OnLevelVisible()
{
    RefreshMaterials();
}

void ALightingMaterialManager::OnBeginLightScenarioChange()
{
    RefreshMaterials();
}

/*
 * Gets current lighting scenario, then for each MeshMaterials entry picks the
 * correct material array (LightsOn/LightsOff/Dawn) and applies via MaterialWorker.
 */
void ALightingMaterialManager::RefreshMaterials()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    ULightScenarioManager* ScenarioMgr = World->GetSubsystem<ULightScenarioManager>();
    ELightingScenario CurrentScenario = ELightingScenario::None;
    if (ScenarioMgr)
    {
        CurrentScenario = ScenarioMgr->GetCurrentLightingScenario();
    }

    for (int32 i = 0; i < MeshMaterials.Num(); ++i)
    {
        FLightMeshMaterialInfo& Info = MeshMaterials[i];

        TArray<UMaterialInterface*>* MaterialsToApply = nullptr;
        switch ((uint8)CurrentScenario)
        {
        case 1: // LightsOn
            MaterialsToApply = &Info.LightsOnMaterials;
            break;
        case 2: // LightsOff
            MaterialsToApply = &Info.LightsOffMaterials;
            break;
        case 3: // Dawn
            MaterialsToApply = &Info.DawnMaterials;
            break;
        default:
            MaterialsToApply = nullptr;
            break;
        }

        MaterialWorker(MaterialsToApply, Info);
    }
}

/* Resolves soft references and applies materials to each StaticMeshActor's mesh component. */
void ALightingMaterialManager::MaterialWorker(TArray<UMaterialInterface*>* MaterialArrayPtr, FLightMeshMaterialInfo& Info)
{
    if (!MaterialArrayPtr)
    {
        return;
    }

    for (int32 i = 0; i < Info.StaticMeshActor.Num(); ++i)
    {
        AStaticMeshActor* MeshActor = Info.StaticMeshActor[i].Get();
        if (!MeshActor)
        {
            MeshActor = Info.StaticMeshActor[i].LoadSynchronous();
        }

        if (!MeshActor)
        {
            continue;
        }

        AStaticMeshActor* VerifiedActor = Cast<AStaticMeshActor>(MeshActor);
        if (!VerifiedActor)
        {
            continue;
        }

        UStaticMeshComponent* MeshComp = VerifiedActor->GetStaticMeshComponent();
        if (!MeshComp)
        {
            continue;
        }

        for (int32 MatIdx = 0; MatIdx < MaterialArrayPtr->Num(); ++MatIdx)
        {
            MeshComp->SetMaterial(MatIdx, (*MaterialArrayPtr)[MatIdx]);
        }
    }
}

/* Looks up Key in each entry's SpecialEventMaterials TMap and applies if found. */
void ALightingMaterialManager::OnActivateSpecialLightingScenerio(const FString& Key)
{
    for (int32 i = 0; i < MeshMaterials.Num(); ++i)
    {
        FLightMeshMaterialInfo& Info = MeshMaterials[i];

        FMaterialArray* Found = Info.SpecialEventMaterials.Find(Key);
        if (Found)
        {
            MaterialWorker(&Found->MaterialArray, Info);
        }
    }
}
