#pragma once
#include "CoreMinimal.h"
#include "Engine/NavigationObjectBase.h"
#include "EFNAFAISpawnType.h"
#include "FNAFAISpawnPoint.generated.h"

UCLASS(Blueprintable)
class FNAF9_API AFNAFAISpawnPoint : public ANavigationObjectBase {
    GENERATED_BODY()
public:
    AFNAFAISpawnPoint();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    EFNAFAISpawnType GetAIType() const;

private:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bUseType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EFNAFAISpawnType AIType;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    bool bIsStagedPoint;
};