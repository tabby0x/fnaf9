#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/DataTable.h"
#include "FNAFDataTableUtils.generated.h"

UCLASS()
class FNAF9_API UDataTableUtils : public UBlueprintFunctionLibrary {
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "DataTable")
    static UDataTable* GetAnimatronicVODataTable(FName RowName);

private:
    static const TCHAR* FREDDY_VO_TABLE_PATH;
};