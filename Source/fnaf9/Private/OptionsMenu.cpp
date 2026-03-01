/* UOptionsMenu -- handles audio/video settings from the pause menu UI.
   ROOT CAUSE OF GRAPHICS RESET BUG: all functions were stubbed empty, so
   RestoreVideoSettings/RestoreAudioSettings never applied to the engine.
   The Blueprint UI would show default/max values and apply them on close. */

#include "OptionsMenu.h"
#include "FNAFGameUserSettings.h"
#include "FNAFGameInstanceBase.h"
#include "AkGameplayStatics.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"

UOptionsMenu::UOptionsMenu() : UUserWidget(FObjectInitializer::Get())
{
}

// Sets 5 Wwise RTPC values for each audio channel
void UOptionsMenu::VolumeSliders(float MasterVolume, float MusicVolume, float VoiceVolume, float JumpscareVolume, float SFXVolume)
{
    UAkGameplayStatics::SetRTPCValue(nullptr, MasterVolume, 0, nullptr, FName(TEXT("uservolume_master")));
    UAkGameplayStatics::SetRTPCValue(nullptr, MusicVolume, 0, nullptr, FName(TEXT("uservolume_music")));
    UAkGameplayStatics::SetRTPCValue(nullptr, VoiceVolume, 0, nullptr, FName(TEXT("uservolume_dialogue")));
    UAkGameplayStatics::SetRTPCValue(nullptr, JumpscareVolume, 0, nullptr, FName(TEXT("uservolume_jumpscare")));
    UAkGameplayStatics::SetRTPCValue(nullptr, SFXVolume, 0, nullptr, FName(TEXT("uservolume_sfx")));
}

/* IDA: SubtitlesValue 0 -> true, 1 or 2 -> false, otherwise pass through SubtitlesOn */
bool UOptionsMenu::UpdateSubtitles(int32 SubtitlesValue, bool SubtitlesOn, bool SubtitlesSmall, bool SubtitlesLarge)
{
    if (SubtitlesValue == 0)
    {
        return true;
    }

    if (SubtitlesValue == 1 || SubtitlesValue == 2)
    {
        return false;
    }

    return SubtitlesOn;
}

/* Called by Blueprint when reverting video settings (cancel/timeout).
   DefaultRes and DefaultColorblind params are NOT used in C++ -- the Blueprint
   handles resolution and colorblind selection through separate calls. */
void UOptionsMenu::RestoreVideoSettings(TEnumAsByte<EWindowMode::Type> DefaultScreenMode, int32 DefaultRes, int32 DefaultColorblind, int32 VisualQuality)
{
    UGameUserSettings* GUS = GEngine->GetGameUserSettings();
    if (!GUS)
    {
        return;
    }

    GUS->SetFullscreenMode(DefaultScreenMode);
    GUS->ApplySettings(false);

    if (VisualQuality <= 2)
    {
        UFNAFGameUserSettings* FNAFSettings = Cast<UFNAFGameUserSettings>(GUS);
        if (FNAFSettings)
        {
            FNAFSettings->VisualQualityLevel = VisualQuality;
        }
    }

    GUS->SetVisualEffectQuality(VisualQuality);
    GUS->ApplySettings(false);
}

/* Called by Blueprint when reverting audio settings. Subtitle params are NOT
   used in C++ -- the Blueprint handles subtitle state through widget bindings. */
void UOptionsMenu::RestoreAudioSettings(float MasterVolume, float MusicVolume, float VoiceVolume, float JumpscareVolume, float SFXVolume, int32 UISubtitlesValue, bool UISubtitlesOn, bool UISubtitlesSmall, bool UISubtitlesLarge)
{
    UGameUserSettings* GUS = GEngine->GetGameUserSettings();

    UAkGameplayStatics::SetRTPCValue(nullptr, MasterVolume, 0, nullptr, FName(TEXT("uservolume_master")));
    UAkGameplayStatics::SetRTPCValue(nullptr, MusicVolume, 0, nullptr, FName(TEXT("uservolume_music")));
    UAkGameplayStatics::SetRTPCValue(nullptr, VoiceVolume, 0, nullptr, FName(TEXT("uservolume_dialogue")));
    UAkGameplayStatics::SetRTPCValue(nullptr, JumpscareVolume, 0, nullptr, FName(TEXT("uservolume_jumpscare")));
    UAkGameplayStatics::SetRTPCValue(nullptr, SFXVolume, 0, nullptr, FName(TEXT("uservolume_sfx")));

    if (GUS)
    {
        GUS->ApplySettings(false);
    }
}

/* Blueprint has already set resolution/quality values through other calls
   before invoking this as the final "commit". */
void UOptionsMenu::ApplyVideoSettings(int32 UIResValue)
{
    UGameUserSettings* GUS = GEngine->GetGameUserSettings();
    if (GUS)
    {
        GUS->ApplySettings(false);
    }
}

// No pseudocode available
void UOptionsMenu::ResolutionSelection(int32 UIValue)
{
}

// No pseudocode available
void UOptionsMenu::PSVideoSettings()
{
}

// No pseudocode available
void UOptionsMenu::ColorBlindSelection(int32 UIColorValue, float Severity)
{
}
