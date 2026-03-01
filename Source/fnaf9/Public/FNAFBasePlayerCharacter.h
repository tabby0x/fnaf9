#pragma once
#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GameFramework/Character.h"
#include "EPlayerPawnType.h"
#include "FNAFPawnTypeProviderInterface.h"
#include "LevelStreamViewpointProvider.h"
#include "FNAFBasePlayerCharacter.generated.h"

class USpringArmComponent;

UCLASS(Blueprintable)
class FNAF9_API AFNAFBasePlayerCharacter : public ACharacter, public IFNAFPawnTypeProviderInterface, public ILevelStreamViewpointProvider {
    GENERATED_BODY()
public:
    AFNAFBasePlayerCharacter();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void FellOutOfWorld(const UDamageType& dmgType) override;

    UFUNCTION(BlueprintCallable)
    void TeleportPlayer(const FVector& WorldLocation, float Yaw);

    UFUNCTION(BlueprintCallable)
    void TeleportPlayerWithCameraLocation(const FVector& CameraWorldLocation, float Yaw);

    UFUNCTION(BlueprintCallable, BlueprintNativeEvent)
    EPlayerPawnType GetPlayerPawnType() const;

    UFUNCTION(BlueprintCallable)
    void GetLastSavedLocationAndRotation(FVector& LastSavedLocation, FRotator& LastSavedRotation);

private:
    UFUNCTION()
    void OnKillZLevelsLoaded();

    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    EPlayerPawnType PawnType;
};