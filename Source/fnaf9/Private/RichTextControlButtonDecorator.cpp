/* URichTextControlButtonDecorator -- rich text decorator that displays
   controller/keyboard button icons inline. Looks up button brushes from a
   DataTable (FRichControlButtonRow), selecting the correct icon based on the
   current input device type. Falls back to DefaultBrush if no controller-specific
   brush is configured. Used in UI text like "<ctrl id=\"Jump\"/>". */

#include "RichTextControlButtonDecorator.h"
#include "RichControlButtonRow.h"
#include "FNAFInputDeviceSystem.h"
#include "Components/RichTextBlock.h"
#include "Engine/DataTable.h"
#include "Framework/Text/RichTextLayoutMarshaller.h"
#include "Framework/Text/SlateTextRun.h"
#include "Widgets/Images/SImage.h"

// Custom ITextDecorator that stores a back-pointer to the decorator for brush lookup
class FRichControlButton : public FRichTextDecorator
{
public:
    FRichControlButton(URichTextBlock* InOwner, URichTextControlButtonDecorator* InDecorator)
        : FRichTextDecorator(InOwner)
        , Decorator(InDecorator)
    {
    }

    virtual bool Supports(const FTextRunParseResults& RunParseResult, const FString& Text) const override
    {
        if (!Decorator || !Decorator->ButtonSet)
        {
            return false;
        }

        const FTextRange* IdRange = RunParseResult.MetaData.Find(TEXT("id"));
        if (IdRange)
        {
            const FString TagId = Text.Mid(IdRange->BeginIndex, IdRange->EndIndex - IdRange->BeginIndex);
            const FSlateBrush* Brush = Decorator->FindButtonBrush(FName(*TagId));
            return Brush != nullptr;
        }

        return false;
    }

    virtual TSharedPtr<SWidget> CreateDecoratorWidget(const FTextRunInfo& RunInfo, const FTextBlockStyle& TextStyle) const override
    {
        if (!Decorator)
        {
            return TSharedPtr<SWidget>();
        }

        const FString* IdValue = RunInfo.MetaData.Find(TEXT("id"));
        if (!IdValue)
        {
            return TSharedPtr<SWidget>();
        }

        const FSlateBrush* Brush = Decorator->FindButtonBrush(FName(**IdValue));
        if (Brush)
        {
            return SNew(SImage).Image(Brush);
        }

        return TSharedPtr<SWidget>();
    }

private:
    URichTextControlButtonDecorator* Decorator;
};

URichTextControlButtonDecorator::URichTextControlButtonDecorator()
    : URichTextBlockDecorator(FObjectInitializer::Get())
{
    ButtonSet = nullptr;
}

TSharedPtr<ITextDecorator> URichTextControlButtonDecorator::CreateDecorator(URichTextBlock* InOwner)
{
    return MakeShareable(new FRichControlButton(InOwner, this));
}

/* Looks up a row by TagOrId in the ButtonSet DataTable. If found, checks
   PerControllerBrush TMap for CurrentInputDeviceType, falling back to DefaultBrush. */
const FSlateBrush* URichTextControlButtonDecorator::FindButtonBrush(FName TagOrId)
{
    if (!ButtonSet)
    {
        return nullptr;
    }

    if (!ButtonSet->RowStruct || !ButtonSet->RowStruct->IsChildOf(FRichControlButtonRow::StaticStruct()))
    {
        return nullptr;
    }

    if (TagOrId.IsNone())
    {
        return nullptr;
    }

    static const FString ContextString(TEXT("FindButtonBrush"));
    FRichControlButtonRow* Row = ButtonSet->FindRow<FRichControlButtonRow>(TagOrId, ContextString, false);
    if (!Row)
    {
        return nullptr;
    }

    const FSlateBrush* ControllerBrush = Row->PerControllerBrush.Find(
        UFNAFInputDeviceSystem::CurrentInputDeviceType);

    if (ControllerBrush)
    {
        return ControllerBrush;
    }

    return &Row->DefaultBrush;
}
