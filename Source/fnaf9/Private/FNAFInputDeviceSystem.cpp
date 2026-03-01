#include "FNAFInputDeviceSystem.h"

ESWGInputDeviceType UFNAFInputDeviceSystem::CurrentInputDeviceType = ESWGInputDeviceType::MouseAndKeyboard;

ESWGInputDeviceType UFNAFInputDeviceSystem::GetCurrentInputDevice() const {
    return CurrentInputDeviceType;
}

UFNAFInputDeviceSystem::UFNAFInputDeviceSystem() {
}

