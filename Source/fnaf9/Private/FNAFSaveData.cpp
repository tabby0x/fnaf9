#include "FNAFSaveData.h"

void UFNAFSaveData::SetHourOfCheckpoint(int32 InHour) {
    Hour = InHour;
}

UFNAFSaveData::UFNAFSaveData() {
    // From IDA: *(_QWORD *)&this->Minute = 0 (zeroes both Minute and Hour together)
    this->Hour = 0;
    this->Minute = 0;
    this->GameIteration = 0;
    this->TotalTimePlayedInSeconds = 0;
    this->bInPowerStation = false;
    this->PowerStationID = 0;

    // From IDA: this->FreddyUpgrades = 0
    this->FreddyUpgrades = FFreddyUpgradeState();

    // From IDA: this->InventorySaveData.DishesBroken = 0
    this->InventorySaveData.DishesBroken = 0;
    // From IDA: this->InventorySaveData.FlashlightInStationID = -1
    this->InventorySaveData.FlashlightInStationID = -1;

    // From IDA: *(_DWORD *)&this->AISaveData.bShatteredChica = 0
    this->AISaveData.bShatteredChica = false;
    this->AISaveData.bAITeleportEnabled = false;
}