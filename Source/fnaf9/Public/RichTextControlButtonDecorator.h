#pragma once
#include "CoreMinimal.h"
#include "Components/RichTextBlockDecorator.h"
#include "RichTextControlButtonDecorator.generated.h"

class UDataTable;
struct FSlateBrush;

UCLASS(Abstract, Blueprintable)
class FNAF9_API URichTextControlButtonDecorator : public URichTextBlockDecorator {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (AllowPrivateAccess = true))
    UDataTable* ButtonSet;

    URichTextControlButtonDecorator();

    // From IDA: looks up row by TagOrId in ButtonSet DataTable,
    // returns controller-specific brush if available, else DefaultBrush
    const FSlateBrush* FindButtonBrush(FName TagOrId);

    // URichTextBlockDecorator override
    virtual TSharedPtr<ITextDecorator> CreateDecorator(URichTextBlock* InOwner) override;
};