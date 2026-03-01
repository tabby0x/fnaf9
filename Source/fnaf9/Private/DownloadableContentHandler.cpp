#include "DownloadableContentHandler.h"

bool UDownloadableContentHandler::HasMountedDLCPack() {
    return true;
}

bool UDownloadableContentHandler::HasDLC() {
    return true;
}

TArray<FString> UDownloadableContentHandler::GetDLCPurchases() {
    return TArray<FString>();
}

UDownloadableContentHandler::UDownloadableContentHandler() {
}

