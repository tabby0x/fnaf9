#include "POIResultBlueprintLibrary.h"

UPOIResultBlueprintLibrary::UPOIResultBlueprintLibrary()
{
}

// VisitTime > -1.0 means visited (ClearPOIVisited sets to -1.0)
bool UPOIResultBlueprintLibrary::HasPOIBeenVisited(const FPointOfInterestRuntimeInfo& Info)
{
    return Info.VisitTime > -1.f;
}