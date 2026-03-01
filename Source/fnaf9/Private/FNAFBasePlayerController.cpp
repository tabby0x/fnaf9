#include "FNAFBasePlayerController.h"
#include "FNAFInputDeviceSystem.h"
#include "Engine/GameInstance.h"

DEFINE_LOG_CATEGORY_STATIC(LogFNAFPlayerController, Log, All);

bool AFNAFBasePlayerController::bDbgEntered = false;
bool AFNAFBasePlayerController::bIsUsingGamepad = false;
int32 AFNAFBasePlayerController::SequenceIndex = 0;

/*
 * Constructor -- Builds two parallel cheat code sequences (gamepad and keyboard).
 * Each sequence has 6 progressively shorter steps. The player must match each
 * step exactly to advance; completing all steps activates debug mode.
 */
AFNAFBasePlayerController::AFNAFBasePlayerController()
{
    SequenceIndex = 0;

    // ---- Gamepad sequence ----
    {
        TArray<FKey> Step0;
        Step0.Add(EKeys::Gamepad_FaceButton_Left);
        Step0.Add(EKeys::Gamepad_FaceButton_Top);
        Step0.Add(EKeys::Gamepad_FaceButton_Right);
        GamepadSequence.Add(Step0);

        TArray<FKey> Step1;
        Step1.Add(EKeys::Gamepad_FaceButton_Left);
        Step1.Add(EKeys::Gamepad_FaceButton_Top);
        GamepadSequence.Add(Step1);

        TArray<FKey> Step2;
        Step2.Add(EKeys::Gamepad_FaceButton_Top);
        Step2.Add(EKeys::Gamepad_FaceButton_Right);
        GamepadSequence.Add(Step2);

        TArray<FKey> Step3;
        Step3.Add(EKeys::Gamepad_FaceButton_Right);
        GamepadSequence.Add(Step3);

        TArray<FKey> Step4;
        Step4.Add(EKeys::Gamepad_FaceButton_Top);
        GamepadSequence.Add(Step4);

        TArray<FKey> Step5;
        Step5.Add(EKeys::Gamepad_FaceButton_Left);
        GamepadSequence.Add(Step5);
    }

    // ---- Keyboard sequence ----
    {
        TArray<FKey> Step0;
        Step0.Add(EKeys::RightShift);
        Step0.Add(EKeys::RightControl);
        Step0.Add(EKeys::A);
        Step0.Add(EKeys::W);
        Step0.Add(EKeys::D);
        KeyboardSequence.Add(Step0);

        TArray<FKey> Step1;
        Step1.Add(EKeys::RightShift);
        Step1.Add(EKeys::RightControl);
        Step1.Add(EKeys::A);
        Step1.Add(EKeys::W);
        KeyboardSequence.Add(Step1);

        TArray<FKey> Step2;
        Step2.Add(EKeys::RightShift);
        Step2.Add(EKeys::RightControl);
        Step2.Add(EKeys::W);
        Step2.Add(EKeys::D);
        KeyboardSequence.Add(Step2);

        TArray<FKey> Step3;
        Step3.Add(EKeys::RightShift);
        Step3.Add(EKeys::RightControl);
        Step3.Add(EKeys::D);
        KeyboardSequence.Add(Step3);

        TArray<FKey> Step4;
        Step4.Add(EKeys::RightShift);
        Step4.Add(EKeys::RightControl);
        Step4.Add(EKeys::W);
        KeyboardSequence.Add(Step4);

        TArray<FKey> Step5;
        Step5.Add(EKeys::RightShift);
        Step5.Add(EKeys::RightControl);
        Step5.Add(EKeys::A);
        KeyboardSequence.Add(Step5);
    }
}

void AFNAFBasePlayerController::BeginPlay()
{
    Super::BeginPlay();
}

static bool IsKeyDown(const TArray<FKey>& KeysDown, const FKey& Key)
{
    for (const FKey& K : KeysDown)
    {
        if (K == Key)
        {
            return true;
        }
    }
    return false;
}

/*
 * InputKey -- On press: tracks key, checks modifier combos, processes debug
 * shortcuts (F6/F7/U/Slash + modifiers), and advances cheat code sequence.
 * On release: removes key from tracked set.
 */
bool AFNAFBasePlayerController::InputKey(FKey Key, EInputEvent EventType, float AmountDepressed, bool bGamepad)
{
    if (EventType == IE_Pressed)
    {
        // Add key to tracked list
        CurrentKeysDown.Add(Key);

        // ---- Check keyboard modifier state ----
        // v19: true if (RightShift+RightControl) OR (LeftShift+LeftControl) are held AND bDbgEntered
        bool bKeyboardModifiers = false;
        if (bDbgEntered)
        {
            bool bRightCombo = IsKeyDown(CurrentKeysDown, EKeys::RightShift)
                && IsKeyDown(CurrentKeysDown, EKeys::RightControl);
            bool bLeftCombo = IsKeyDown(CurrentKeysDown, EKeys::LeftShift)
                && IsKeyDown(CurrentKeysDown, EKeys::LeftControl);
            bKeyboardModifiers = bRightCombo || bLeftCombo;
        }

        // ---- Check gamepad modifier state ----
        // v24: true if all 4 face buttons are held AND bDbgEntered
        bool bGamepadModifiers = false;
        if (bDbgEntered)
        {
            bGamepadModifiers = IsKeyDown(CurrentKeysDown, EKeys::Gamepad_FaceButton_Bottom)
                && IsKeyDown(CurrentKeysDown, EKeys::Gamepad_FaceButton_Left)
                && IsKeyDown(CurrentKeysDown, EKeys::Gamepad_FaceButton_Right)
                && IsKeyDown(CurrentKeysDown, EKeys::Gamepad_FaceButton_Top);
        }

        // ---- Debug shortcuts ----

        // Toggle Dev UI: F6 + keyboard mods, OR 5 gamepad buttons (4 face + L1)
        if (bKeyboardModifiers && IsKeyDown(CurrentKeysDown, EKeys::F6))
        {
            OnToggleDevUI();
        }
        else if (bGamepadModifiers && IsKeyDown(CurrentKeysDown, EKeys::Gamepad_LeftShoulder)
            && CurrentKeysDown.Num() == 5)
        {
            OnToggleDevUI();
        }

        // Toggle Localization QA: / + keyboard mods, OR 5 gamepad buttons (4 face + R1)
        if (bKeyboardModifiers && IsKeyDown(CurrentKeysDown, EKeys::Slash))
        {
            OnToggleLocalizationQA();
        }
        else if (bGamepadModifiers && IsKeyDown(CurrentKeysDown, EKeys::Gamepad_RightShoulder)
            && CurrentKeysDown.Num() == 5)
        {
            OnToggleLocalizationQA();
        }

        // Toggle Flight Mode: F7 + keyboard mods, OR 5 gamepad buttons (4 face + R3)
        if (bKeyboardModifiers && IsKeyDown(CurrentKeysDown, EKeys::F7))
        {
            OnToggleFlightMode();
        }
        else if (bGamepadModifiers && IsKeyDown(CurrentKeysDown, EKeys::Gamepad_RightThumbstick)
            && CurrentKeysDown.Num() == 5)
        {
            OnToggleFlightMode();
        }

        // Unlock Everything: U + keyboard mods (no gamepad equivalent)
        if (bKeyboardModifiers && IsKeyDown(CurrentKeysDown, EKeys::U))
        {
            OnUnlockEverything();
        }

        // ---- Cheat code sequence matching ----
        {
            const TArray<TArray<FKey>>& Sequence = bGamepad ? GamepadSequence : KeyboardSequence;
            int32 SeqIdx = SequenceIndex;

            if (SeqIdx >= Sequence.Num())
            {
                SequenceIndex = 0;
            }
            else
            {
                const TArray<FKey>& Step = Sequence[SeqIdx];

                // Check if the pressed key exists in the current step
                bool bKeyInStep = IsKeyDown(Step, Key);

                if (!bKeyInStep)
                {
                    // Wrong key — reset sequence
                    SequenceIndex = 0;
                }
                else
                {
                    // Verify ALL currently held keys are in the step (no extras)
                    bool bAllKeysValid = true;
                    for (const FKey& HeldKey : CurrentKeysDown)
                    {
                        if (!IsKeyDown(Step, HeldKey))
                        {
                            bAllKeysValid = false;
                            break;
                        }
                    }

                    if (bAllKeysValid && CurrentKeysDown.Num() == Step.Num())
                    {
                        // Exact match — advance to next step
                        SequenceIndex = SeqIdx + 1;

                        // Check if sequence is complete
                        // IDA: compares against GamepadSequence.Num() for both paths
                        if (SequenceIndex >= GamepadSequence.Num())
                        {
                            SequenceIndex = 0;
                            bDbgEntered = true;
                            OnDebugSequenceEntered();
                        }
                    }
                }
            }
        }
    }
    else if (EventType == IE_Released)
    {
        // Remove key from tracked list
        CurrentKeysDown.RemoveAll([&Key](const FKey& K) { return K == Key; });
    }

    // Update gamepad tracking and forward to parent
    UpdateUsingGamepadState(bGamepad);
    return APlayerController::InputKey(Key, EventType, AmountDepressed, bGamepad);
}

// Gamepad threshold: 0.5, Keyboard threshold: 1.0
bool AFNAFBasePlayerController::InputAxis(FKey Key, float Delta, float DeltaTime, int32 NumSamples, bool bGamepad)
{
    if (bGamepad)
    {
        if (Delta > 0.5f)
        {
            UpdateUsingGamepadState(bGamepad);
        }
    }
    else
    {
        if (Delta > 1.0f)
        {
            UpdateUsingGamepadState(bGamepad);
        }
    }

    return APlayerController::InputAxis(Key, Delta, DeltaTime, NumSamples, bGamepad);
}

/*
 * PlayerTick -- Checks minimum hold time across 9 gamepad buttons. If all held
 * >= 3.0s (or bDbgEntered and any hold > 0.0), activates debug mode.
 */
void AFNAFBasePlayerController::PlayerTick(float DeltaTime)
{
    APlayerController::PlayerTick(DeltaTime);

    // Get minimum hold time across all 9 debug buttons
    float MinTime = GetInputKeyTimeDown(EKeys::Gamepad_FaceButton_Top);
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_FaceButton_Bottom));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_FaceButton_Left));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_FaceButton_Right));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_LeftShoulder));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_LeftTrigger));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_LeftThumbstick));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_RightShoulder));
    MinTime = FMath::Min(MinTime, GetInputKeyTimeDown(EKeys::Gamepad_RightTrigger));

    // Hold all 9 buttons for 3+ seconds to enter debug mode
    // Once entered, any hold > 0 re-triggers (for re-entry after reset)
    if (MinTime >= 3.0f || (bDbgEntered && MinTime > 0.0f))
    {
        bDbgEntered = true;
        OnDebugSequenceEntered();
    }
}

/*
 * UpdateUsingGamepadState -- Clears tracked keys, resets cheat sequence,
 * updates static state, notifies Blueprint and input device subsystem.
 */
void AFNAFBasePlayerController::UpdateUsingGamepadState(bool bUsingGamepad)
{
    if (bUsingGamepad == bIsUsingGamepad)
    {
        return;
    }

    // Clear all tracked keys
    CurrentKeysDown.Empty();
    SequenceIndex = 0;

    // Update static state
    bIsUsingGamepad = bUsingGamepad;

    // Notify Blueprint
    OnUsingGamepadChanged();

    // Broadcast delegate
    OnControlTypeChanged.Broadcast(bIsUsingGamepad);

    // Update input device subsystem
    // IDA shows: UFNAFInputDeviceSystem::CurrentInputDeviceType = bIsUsingGamepad  (static member)
    //            then broadcasts OnInputDeviceTypeChanged
    // CurrentInputDeviceType is a static ESWGInputDeviceType not in the dumped header —
    // needs to be added to UFNAFInputDeviceSystem.h as:
    //   static ESWGInputDeviceType CurrentInputDeviceType;
    UGameInstance* GameInstance = GetGameInstance();
    if (GameInstance)
    {
        UFNAFInputDeviceSystem* InputDeviceSystem = GameInstance->GetSubsystem<UFNAFInputDeviceSystem>();
        if (InputDeviceSystem)
        {
            // IDA: direct assignment of bool to enum. false(0)=MouseAndKeyboard, true(1)=XBox
            UFNAFInputDeviceSystem::CurrentInputDeviceType =
                static_cast<ESWGInputDeviceType>(bIsUsingGamepad);
            InputDeviceSystem->OnInputDeviceTypeChanged.Broadcast();
        }
    }
}

bool AFNAFBasePlayerController::IsUsingGamepad() const
{
    return bIsUsingGamepad;
}

TArray<FKey> AFNAFBasePlayerController::GetCurrentKeysDown() const
{
    return CurrentKeysDown;
}

bool AFNAFBasePlayerController::AnyOtherKeysDown(FKey Key) const
{
    if (CurrentKeysDown.Num() != 1)
    {
        return true;
    }
    return CurrentKeysDown[0] != Key;
}

void AFNAFBasePlayerController::OnUsingGamepadChanged_Implementation()
{
}