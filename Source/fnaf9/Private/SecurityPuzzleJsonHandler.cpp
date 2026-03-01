/*
 * Static utility for reading/writing security node puzzle definitions
 * to/from Content/Data/NonAssets/SecurityNodePuzzles.json.
 * The security nodes in RUIN are the hacking puzzles where you rotate/flip
 * hologram pieces to match a pattern.
 */

#include "SecurityPuzzleJsonHandler.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"
#include <JsonUtilities/Public/JsonObjectConverter.h>

USecurityPuzzleJsonHandler::USecurityPuzzleJsonHandler()
{
}

//
//   1. Start with FileContents = "["
//   2. For each puzzle in array:
//      a. Copy struct fields (IDA shows manual field-by-field copy)
//      b. FJsonObjectConverter::UStructToJsonObjectString to convert to JSON
//      c. Append JSON string to FileContents
//      d. Append "," separator (except after last element — IDA breaks
//         out of loop early if JsonString is empty/1 char)
//   3. Append "]"
//   4. Build path: FPaths::ProjectContentDir() / "Data/NonAssets/SecurityNodePuzzles.json"
//   5. Check file exists via IPlatformFile — if not, return false
//   6. FFileHelper::SaveStringToFile — return true on success, false on failure
bool USecurityPuzzleJsonHandler::WriteAllPuzzleDataToFile(TArray<FSecurityNodePuzzle> Puzzles)
{
    FString FileContents = TEXT("[");

    for (int32 i = 0; i < Puzzles.Num(); ++i)
    {
        FString JsonString;
        if (!FJsonObjectConverter::UStructToJsonObjectString(
            FSecurityNodePuzzle::StaticStruct(),
            &Puzzles[i],
            JsonString,
            0, 0))
        {
            continue;
        }

        if (JsonString.Len() <= 0)
        {
            continue;
        }

        FileContents.Append(JsonString);

        if (i < Puzzles.Num() - 1)
        {
            FileContents.Append(TEXT(","));
        }
    }

    FileContents.Append(TEXT("]"));

    FString FilePath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data/NonAssets/SecurityNodePuzzles.json"));

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*FilePath))
    {
        return false;
    }

    return FFileHelper::SaveStringToFile(FileContents, *FilePath);
}

//
//   1. Build path: FPaths::ProjectContentDir() / "Data/NonAssets/SecurityNodePuzzles.json"
//   2. Check file exists via IPlatformFile
//   3. If exists: FFileHelper::LoadFileToString → return contents
//   4. If not: return empty string
FString USecurityPuzzleJsonHandler::ReadSecurityNodePuzzleData()
{
    FString FilePath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data/NonAssets/SecurityNodePuzzles.json"));

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*FilePath))
    {
        return FString();
    }

    FString FileContents;
    FFileHelper::LoadFileToString(FileContents, *FilePath);
    return FileContents;
}

//
// Parses a JSON array string into a TArray of FSecurityNodePuzzle structs.
void USecurityPuzzleJsonHandler::GenerateStructsArrayFromJsonStringSecurityNodePuzzle(const FString& JsonString, TArray<FSecurityNodePuzzle>& Puzzles)
{
    FJsonObjectConverter::JsonArrayStringToUStruct<FSecurityNodePuzzle>(JsonString, &Puzzles, 0, 0);
}