#pragma once
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "OnControlTypeChangedDelegate.h"
#include "FNAFBasePlayerController.generated.h"

UCLASS(Blueprintable)
class FNAF9_API AFNAFBasePlayerController : public APlayerController {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable, BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    FOnControlTypeChanged OnControlTypeChanged;

    AFNAFBasePlayerController();

    virtual bool InputKey(FKey Key, EInputEvent EventType, float AmountDepressed, bool bGamepad) override;
    virtual bool InputAxis(FKey Key, float Delta, float DeltaTime, int32 NumSamples, bool bGamepad) override;
    virtual void PlayerTick(float DeltaTime) override;
	virtual void BeginPlay() override;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool IsUsingGamepad() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    TArray<FKey> GetCurrentKeysDown() const;

    UFUNCTION(BlueprintCallable, BlueprintPure)
    bool AnyOtherKeysDown(FKey Key) const;

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void GiveVIPAchievement();

protected:
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent)
    void OnUsingGamepadChanged();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnUnlockEverything();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnTriggerVannyScare();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnToggleLocalizationQA();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnToggleFlightMode();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnToggleDevUI();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnToggleCinemaMode();

    UFUNCTION(BlueprintCallable, BlueprintImplementableEvent)
    void OnDebugSequenceEntered();

private:
    /*
     * Debug cheat code system -- two parallel sequences (gamepad/keyboard), each
     * with 6 steps of simultaneous key combos. Completing all steps sets bDbgEntered
     * and fires OnDebugSequenceEntered.
     */
    TArray<TArray<FKey>> GamepadSequence;
    TArray<TArray<FKey>> KeyboardSequence;

    // Currently held keys, tracked manually via InputKey press/release
    TArray<FKey> CurrentKeysDown;

    // Current position in the cheat sequence
    static int32 SequenceIndex;

    // Static flags shared across all controller instances
    static bool bDbgEntered;
    static bool bIsUsingGamepad;

    // Internal helper — updates gamepad state, clears keys, broadcasts delegates
    void UpdateUsingGamepadState(bool bUsingGamepad);
};