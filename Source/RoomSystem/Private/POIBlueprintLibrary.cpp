#include "POIBlueprintLibrary.h"
#include "RoomAreaBase.h"

UPOIBlueprintLibrary::UPOIBlueprintLibrary()
{
}

void UPOIBlueprintLibrary::SetPOIVisited(const FPOIIndex& Index)
{
    if (Index.Room && Index.Room->IsValidLowLevel())
    {
        Index.Room->SetPOIVisited(Index.Index);
    }
}

bool UPOIBlueprintLibrary::IsValid(const FPOIIndex& Index)
{
    if (!Index.Room || !Index.Room->IsValidLowLevel())
    {
        return false;
    }

    int32 Idx = Index.Index;
    if (Idx >= 0 && Idx < Index.Room->GetPointsOfInterestInfo().Num())
    {
        return true;
    }

    return false;
}

// ClearPOIVisited sets VisitTime to -1.0f, so "visited" means > -1.0 (not > 0.0)
bool UPOIBlueprintLibrary::HasPOIBeenVisited(const FPOIIndex& Index)
{
    FPointOfInterestRuntimeInfo Info;

    if (Index.Room && Index.Room->IsValidLowLevel())
    {
        Info = Index.Room->GetPointOfInterestInfoByIndex(Index.Index);
    }
    else
    {
        Info = FPointOfInterestRuntimeInfo();
    }

    return Info.VisitTime > -1.f;
}

void UPOIBlueprintLibrary::GetPOIResultsFromIndices(const TArray<FPOIIndex>& Indices, TArray<FPOIResult>& Results)
{
    Results.Reserve(Indices.Num());

    for (const FPOIIndex& Idx : Indices)
    {
        FPointOfInterestRuntimeInfo Info = Idx.Room->GetPointOfInterestInfoByIndex(Idx.Index);

        FPOIResult Result;
        Result.Index = Idx;
        Result.Info = Info;
        Results.Add(Result);
    }
}

FPointOfInterestRuntimeInfo UPOIBlueprintLibrary::GetPOIInfoFromIndex(const FPOIIndex& Index)
{
    if (Index.Room && Index.Room->IsValidLowLevel())
    {
        return Index.Room->GetPointOfInterestInfoByIndex(Index.Index);
    }

    return FPointOfInterestRuntimeInfo();
}

void UPOIBlueprintLibrary::CreatePOIIndicesFromResults(const TArray<FPOIResult>& Results, TArray<FPOIIndex>& Indices)
{
    Indices.Reserve(Results.Num());

    for (const FPOIResult& Result : Results)
    {
        Indices.Add(Result.Index);
    }
}