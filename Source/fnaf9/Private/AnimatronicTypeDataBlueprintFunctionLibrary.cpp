/* Utility functions for converting between TArray<EFNAFAISpawnType> and
   TArray<FAnimatronicTypeData>. Used throughout the AI system when
   Blueprints need to bridge between the simple enum arrays and the
   full type data structs. */

#include "AnimatronicTypeDataBlueprintFunctionLibrary.h"

UAnimatronicTypeDataBlueprintFunctionLibrary::UAnimatronicTypeDataBlueprintFunctionLibrary()
{
}

// Wraps each spawn type enum with the shared SubType into a full FAnimatronicTypeData struct
TArray<FAnimatronicTypeData> UAnimatronicTypeDataBlueprintFunctionLibrary::GetAnimatronicTypeDataArrayFromFNAFAISpawnTypeArray(
    TArray<EFNAFAISpawnType> SpawnTypes,
    EFNAFAISubType SpawnSubType)
{
    TArray<FAnimatronicTypeData> Result;
    Result.Reserve(SpawnTypes.Num());

    for (int32 i = 0; i < SpawnTypes.Num(); ++i)
    {
        FAnimatronicTypeData TypeData;
        TypeData.AIType = SpawnTypes[i];
        TypeData.AISubType = SpawnSubType;
        Result.Add(TypeData);
    }

    return Result;
}

// Inverse of above -- extracts AIType from each FAnimatronicTypeData
TArray<EFNAFAISpawnType> UAnimatronicTypeDataBlueprintFunctionLibrary::GetFNAFAISpawnTypeArrayFromAnimatronicTypeDataArray(
    TArray<FAnimatronicTypeData> SpawnTypes)
{
    TArray<EFNAFAISpawnType> Result;
    Result.Reserve(SpawnTypes.Num());

    for (int32 i = 0; i < SpawnTypes.Num(); ++i)
    {
        Result.Add(SpawnTypes[i].AIType);
    }

    return Result;
}
