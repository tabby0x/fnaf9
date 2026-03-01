#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LightMeshMaterialInfo.h"
#include "LightingMaterialManager.generated.h"

class ULevelStreaming;

UCLASS(Blueprintable)
class FNAF9_API ALightingMaterialManager : public AActor {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    TArray<FLightMeshMaterialInfo> MeshMaterials;

    ALightingMaterialManager();

    virtual void BeginPlay() override;

private:
    // Hidden helpers restored from IDA
    void RefreshMaterials();
    void MaterialWorker(TArray<UMaterialInterface*>* MaterialArrayPtr, FLightMeshMaterialInfo& Info);

    UFUNCTION(BlueprintCallable)
    void OnLevelVisible();

    UFUNCTION(BlueprintCallable)
    void OnBeginLightScenarioChange();

public:
    UFUNCTION(BlueprintCallable)
    void OnActivateSpecialLightingScenerio(const FString& Key);

};