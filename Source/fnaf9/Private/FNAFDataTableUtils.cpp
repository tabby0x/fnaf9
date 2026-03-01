#include "FNAFDataTableUtils.h"
#include "UObject/SoftObjectPath.h"

// TODO: Find the actual asset path in IDA (search for FREDDY_VO_TABLE_PATH global string).
const TCHAR* UDataTableUtils::FREDDY_VO_TABLE_PATH = TEXT("/Game/Blueprints/AnimationBlueprints/Freddy_VO_Small_DT.Freddy_VO_Small_DT");

UDataTable* UDataTableUtils::GetAnimatronicVODataTable(FName RowName)
{
    FSoftObjectPath SoftDataTablePath(FREDDY_VO_TABLE_PATH);

    // Try resolve (already in memory)
    UObject* Resolved = SoftDataTablePath.ResolveObject();
    if (UDataTable* AsTable = Cast<UDataTable>(Resolved))
    {
        if (AsTable->IsValidLowLevel())
        {
            return AsTable;
        }
    }

    // Synchronous load
    if (UDataTable* AsTable = Cast<UDataTable>(SoftDataTablePath.TryLoad()))
    {
        return AsTable;
    }

    return nullptr;
}