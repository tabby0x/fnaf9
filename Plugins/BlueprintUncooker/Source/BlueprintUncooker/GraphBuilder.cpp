#include "GraphBuilder.h"

#include "UObject/Script.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Engine/TimelineTemplate.h"
#include "Components/TimelineComponent.h"
#include "Curves/CurveFloat.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Self.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_TemporaryVariable.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Select.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_Timeline.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_GetArrayItem.h"
#include "UObject/UnrealType.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "BlueprintNodeSpawner.h"

#define LOCTEXT_NAMESPACE "BlueprintUncooker"

DEFINE_LOG_CATEGORY_STATIC(LogGraphBuilder, Log, All);

// Construction

FGraphBuilder::FGraphBuilder()
{
}

void FGraphBuilder::Warn(const FString& Message)
{
	Warnings.Add(Message);
	UE_LOG(LogGraphBuilder, Warning, TEXT("%s"), *Message);
}

// Pin Helper Functions

// After any pin connection, resolve wildcard pins to match the connected typed pin.
// UE4's BP compiler rejects wildcard↔typed connections even when schema allows them.
static void ResolveWildcardAfterConnect(UEdGraphPin* A, UEdGraphPin* B)
{
	if (!A || !B) return;

	UEdGraphPin* Output = (A->Direction == EGPD_Output) ? A : B;
	UEdGraphPin* Input = (A->Direction == EGPD_Output) ? B : A;

	bool bOutputWild = Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
	bool bInputWild = Input->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;

	if (bOutputWild && !bInputWild)
	{
		// Preserve container type — if output is wildcard array, keep it array
		EPinContainerType OrigContainer = Output->PinType.ContainerType;
		Output->PinType = Input->PinType;
		if (OrigContainer == EPinContainerType::Array && Input->PinType.ContainerType != EPinContainerType::Array)
		{
			Output->PinType.ContainerType = OrigContainer;
		}
	}
	else if (bInputWild && !bOutputWild)
	{
		EPinContainerType OrigContainer = Input->PinType.ContainerType;
		Input->PinType = Output->PinType;
		if (OrigContainer == EPinContainerType::Array && Output->PinType.ContainerType != EPinContainerType::Array)
		{
			Input->PinType.ContainerType = OrigContainer;
		}
	}
}

bool FGraphBuilder::TryConnect(UEdGraphPin* A, UEdGraphPin* B)
{
	if (!A || !B) return false;

	// Never connect pins on the same node
	if (A->GetOwningNode() == B->GetOwningNode()) return false;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (Schema->TryCreateConnection(A, B))
	{
		ResolveWildcardAfterConnect(A, B);
		return true;
	}

	// If direct connection fails, try the reverse direction
	if (Schema->TryCreateConnection(B, A))
	{
		ResolveWildcardAfterConnect(B, A);
		return true;
	}

	return false;
}

bool FGraphBuilder::ForceConnect(UEdGraphPin* A, UEdGraphPin* B)
{
	if (!A || !B) return false;
	if (A->GetOwningNode() == B->GetOwningNode()) return false;

	// Determine output/input
	UEdGraphPin* Output = (A->Direction == EGPD_Output) ? A : B;
	UEdGraphPin* Input = (A->Direction == EGPD_Output) ? B : A;
	if (Output->Direction != EGPD_Output || Input->Direction != EGPD_Input) return false;

	// ── Handle wildcard pins FIRST ─────────────────────────────────
	// Schema validation would reject wildcard→typed connections and log
	// spurious "Can't connect pins" warnings. Handle these before TryConnect.
	bool bOutputWild = Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
	bool bInputWild = Input->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;

	if (bOutputWild && !bInputWild)
	{
		Output->PinType = Input->PinType;
		Output->MakeLinkTo(Input);
		// Notify the node that owns the wildcard pin so it can propagate
		// the resolved type to other wildcard pins (e.g., Array GET Item→Array)
		Output->GetOwningNode()->PinConnectionListChanged(Output);
		return true;
	}
	else if (bInputWild && !bOutputWild)
	{
		Input->PinType = Output->PinType;
		Output->MakeLinkTo(Input);
		Input->GetOwningNode()->PinConnectionListChanged(Input);
		return true;
	}

	// ── Try schema-validated connection ────────────────────────────
	if (TryConnect(A, B)) return true;

	// Schema validation failed — force the connection with MakeLinkTo.
	// This is used for context wiring where bytecode guarantees correctness
	// but schema can't validate because types aren't fully resolved on the uncooked class.

	// ── Auto-cast: both pins are PC_Object but incompatible types ──────
	// Instead of forcing an invalid connection that breaks on save/reload,
	// insert a pure DynamicCast node between them when needed.
	if (Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Object &&
		Input->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		UClass* OutClass = Cast<UClass>(Output->PinType.PinSubCategoryObject.Get());
		UClass* InClass = Cast<UClass>(Input->PinType.PinSubCategoryObject.Get());

		// If either class is null (e.g., retyped from wildcard), just force connect
		if (!OutClass || !InClass)
		{
			Output->MakeLinkTo(Input);
			return true;
		}

		// Same class — just force connect
		// Also compare by name as fallback: cooked vs uncooked UClass pointers
		// may differ even for the same logical class (e.g., SaveGameUI_C).
		if (OutClass == InClass ||
			OutClass->GetFName() == InClass->GetFName())
		{
			Output->MakeLinkTo(Input);
			return true;
		}

		// Upcast (output is derived from input) — always valid, no cast needed
		// e.g., ThrowableObject* → Actor* parameter
		if (OutClass->IsChildOf(InClass))
		{
			Output->MakeLinkTo(Input);
			return true;
		}

		// Downcast (input is more derived than output) — insert cast
		// e.g., Actor* return → ThrowableObject* variable
		if (InClass->IsChildOf(OutClass))
		{
			UClass* CastTarget = InClass;
			UEdGraph* Graph = Output->GetOwningNode()->GetGraph();
			if (Graph)
			{
				UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
				CastNode->CreateNewGuid();
				CastNode->TargetType = CastTarget;
				CastNode->SetPurity(true);
				CastNode->NodePosX = (Output->GetOwningNode()->NodePosX + Input->GetOwningNode()->NodePosX) / 2;
				CastNode->NodePosY = Output->GetOwningNode()->NodePosY - 50;
				CastNode->AllocateDefaultPins();
				Graph->AddNode(CastNode, false, false);

				UEdGraphPin* CastObjectPin = CastNode->GetCastSourcePin();
				if (CastObjectPin)
				{
					Output->MakeLinkTo(CastObjectPin);
				}

				UEdGraphPin* CastResultPin = CastNode->GetCastResultPin();
				if (CastResultPin)
				{
					CastResultPin->MakeLinkTo(Input);
					return true;
				}
			}
		}

		// Unrelated classes (neither inherits from the other) — force connect
		// without a cast. Inserting a cast between unrelated types (e.g., Pawn →
		// MasterUI) produces "would always fail" compiler errors that are worse
		// than a type mismatch. The bytecode guarantees this connection is valid
		// at runtime (likely via interface or dynamic dispatch).
		Output->MakeLinkTo(Input);
		return true;
	}

	// ── Auto-cast: PC_Object ↔ PC_Interface ────────────────────────────
	if ((Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Object &&
		Input->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface) ||
		(Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface &&
			Input->PinType.PinCategory == UEdGraphSchema_K2::PC_Object))
	{
		UClass* TargetClass = Cast<UClass>(Input->PinType.PinSubCategoryObject.Get());
		if (TargetClass)
		{
			UEdGraph* Graph = Output->GetOwningNode()->GetGraph();
			if (Graph)
			{
				UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
				CastNode->CreateNewGuid();
				CastNode->TargetType = TargetClass;
				CastNode->SetPurity(true);
				CastNode->NodePosX = (Output->GetOwningNode()->NodePosX + Input->GetOwningNode()->NodePosX) / 2;
				CastNode->NodePosY = Output->GetOwningNode()->NodePosY - 50;
				CastNode->AllocateDefaultPins();
				Graph->AddNode(CastNode, false, false);

				UEdGraphPin* CastObjectPin = CastNode->GetCastSourcePin();
				if (CastObjectPin) Output->MakeLinkTo(CastObjectPin);

				UEdGraphPin* CastResultPin = CastNode->GetCastResultPin();
				if (CastResultPin)
				{
					CastResultPin->MakeLinkTo(Input);
					return true;
				}
			}
		}
	}

	// Fallback: force direct connection (may produce compiler warnings)
	Output->MakeLinkTo(Input);
	return true;
}

void FGraphBuilder::SanitizeGraphConnections(UEdGraph* Graph)
{
	if (!Graph) return;

	int32 BrokenCount = 0;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;

			// Check all linked pins for direction mismatches
			for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
			{
				UEdGraphPin* LinkedPin = Pin->LinkedTo[i];
				if (!LinkedPin) continue;

				if (Pin->Direction == LinkedPin->Direction)
				{
					// Same direction = invalid connection. Break it.
					Pin->LinkedTo.RemoveAt(i);

					// Also remove from the other side
					LinkedPin->LinkedTo.Remove(Pin);

					BrokenCount++;

					UE_LOG(LogGraphBuilder, Warning,
						TEXT("Sanitize: removed direction-mismatched link between '%s' (%s) and '%s' (%s)"),
						*Pin->GetDisplayName().ToString(),
						*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
						*LinkedPin->GetDisplayName().ToString(),
						*LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
		}
	}

	if (BrokenCount > 0)
	{
		UE_LOG(LogGraphBuilder, Warning,
			TEXT("Sanitize: removed %d direction-mismatched connection(s) from graph '%s'"),
			BrokenCount, *Graph->GetName());
	}
}

bool FGraphBuilder::ApplyTempLiteralToPin(TSharedPtr<FDecompiledExpr> Expr, UEdGraphPin* Pin)
{
	if (!Expr || !Pin) return false;

	// Check if this expression is a local/instance variable referencing a stored Temp_ literal
	FName VarName = NAME_None;
	if ((Expr->Token == EX_LocalVariable || Expr->Token == EX_InstanceVariable ||
		Expr->Token == EX_LocalOutVariable || Expr->Token == EX_DefaultVariable) &&
		Expr->PropertyRef)
	{
		VarName = Expr->PropertyRef->GetFName();
	}

	if (VarName == NAME_None) return false;

	// Multi-assigned variables (loop counters, accumulators) must not be resolved
	// as literals — their value changes at runtime.
	if (MultiAssignedTempVars.Contains(VarName)) return false;

	TSharedPtr<FDecompiledExpr>* StoredLiteral = TempLiteralMap.Find(VarName);
	if (!StoredLiteral || !(*StoredLiteral)) return false;

	TSharedPtr<FDecompiledExpr> LitExpr = *StoredLiteral;

	// Apply the literal value to the pin, matching the logic from function arg processing
	switch (LitExpr->Token)
	{
	case EX_ObjectConst:
		if (LitExpr->ObjectRef)
		{
			Pin->DefaultObject = LitExpr->ObjectRef;
		}
		return true;

	case EX_NoObject:
		Pin->DefaultValue = TEXT("");
		return true;

	case EX_Self:
		// Self reference — leave pin unconnected
		return true;

	case EX_ByteConst:
	case EX_IntConst:
	case EX_IntConstByte:
	case EX_IntZero:
	case EX_IntOne:
	{
		int64 EnumVal = 0;
		if (LitExpr->Token == EX_ByteConst)
			EnumVal = LitExpr->ByteValue;
		else
			EnumVal = LitExpr->IntValue;

		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
			Pin->PinType.PinSubCategoryObject.IsValid())
		{
			UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
			if (PinEnum)
			{
				FString EnumName = PinEnum->GetNameStringByValue(EnumVal);
				if (!EnumName.IsEmpty())
				{
					Pin->DefaultValue = EnumName;
					return true;
				}
			}
		}
		Pin->DefaultValue = LitExpr->GetLiteralAsString();
		return true;
	}

	default:
	{
		FString DefaultVal = LitExpr->GetLiteralAsString();
		if (!DefaultVal.IsEmpty())
		{
			Pin->DefaultValue = DefaultVal;
		}
		return true;
	}
	}
}

UEdGraphPin* FGraphBuilder::FindExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, FName PinName)
{
	if (!Node) return nullptr;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			if (PinName == NAME_None || Pin->PinName == PinName)
			{
				return Pin;
			}
		}
	}

	// Fallback: try space-stripped, case-insensitive match (handles "LoopBody" vs "Loop Body" etc.)
	if (PinName != NAME_None)
	{
		FString TargetStr = PinName.ToString().Replace(TEXT(" "), TEXT(""));
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				FString PinStr = Pin->PinName.ToString().Replace(TEXT(" "), TEXT(""));
				if (PinStr.Equals(TargetStr, ESearchCase::IgnoreCase))
				{
					return Pin;
				}
			}
		}
	}

	return nullptr;
}

UEdGraphPin* FGraphBuilder::FindDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, FName PinName)
{
	if (!Node) return nullptr;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == Direction && Pin->PinName == PinName &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FGraphBuilder::FindFirstDataOutputPin(UEdGraphNode* Node)
{
	if (!Node) return nullptr;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}
	return nullptr;
}

FName FGraphBuilder::GetPinNameForFunctionParam(UFunction* Function, int32 ParamIndex)
{
	if (!Function) return NAME_None;

	int32 Index = 0;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Prop = *It;
		// Skip the return value
		if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			continue;

		if (Index == ParamIndex)
		{
			return Prop->GetFName();
		}
		Index++;
	}
	return NAME_None;
}

// Top-level Blueprint Building

UBlueprint* FGraphBuilder::BuildBlueprint(
	UClass* OriginalClass,
	const TArray<FDecompiledFunction>& Functions,
	UPackage* TargetPackage,
	const FString& BlueprintName)
{
	if (!OriginalClass || !TargetPackage)
	{
		Warn(TEXT("Null class or package"));
		return nullptr;
	}

	// Determine parent class
	UClass* ParentClass = OriginalClass->GetSuperClass();
	if (!ParentClass)
	{
		ParentClass = AActor::StaticClass();
		Warn(TEXT("Could not determine parent class, defaulting to AActor"));
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Creating Blueprint '%s' with parent '%s'"),
		*BlueprintName, *ParentClass->GetName());

	// Create the Blueprint asset
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		TargetPackage,
		FName(*BlueprintName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		Warn(TEXT("FKismetEditorUtilities::CreateBlueprint returned null"));
		return nullptr;
	}

	if (!BuildBlueprintCore(NewBP, OriginalClass, Functions))
	{
		return nullptr;
	}

	// Move interface function graphs from BP->FunctionGraphs into the matching
	// InterfaceDesc.Graphs entries. During compilation, ConformImplementedInterfaces
	// calls ConformInterfaceByName which checks InterfaceDesc.Graphs to see if
	// a graph already exists for each interface function. If not found there, it
	// calls CreateNewGraph — which asserts because a graph with that name already
	// exists in BP->FunctionGraphs (created by BuildFunctionGraph).
	// Moving them into InterfaceDesc.Graphs prevents the collision.
	for (FBPInterfaceDescription& InterfaceDesc : NewBP->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface) continue;

		for (TFieldIterator<UFunction> FuncIt(InterfaceDesc.Interface); FuncIt; ++FuncIt)
		{
			FName FuncName = FuncIt->GetFName();

			for (int32 i = NewBP->FunctionGraphs.Num() - 1; i >= 0; --i)
			{
				if (NewBP->FunctionGraphs[i]->GetFName() == FuncName)
				{
					InterfaceDesc.Graphs.Add(NewBP->FunctionGraphs[i]);
					NewBP->FunctionGraphs.RemoveAt(i);
					UE_LOG(LogGraphBuilder, Log, TEXT("Moved interface function '%s' to InterfaceDesc.Graphs"),
						*FuncName.ToString());
					break;
				}
			}
		}
	}

	return NewBP;
}

UBlueprint* FGraphBuilder::LiveBuildBlueprint(
	UClass* OriginalClass,
	const TArray<FDecompiledFunction>& Functions,
	UBlueprintGeneratedClass* ExistingBPGC)
{
	if (!OriginalClass || !ExistingBPGC)
	{
		Warn(TEXT("Null class or BPGC for LiveBuildBlueprint"));
		return nullptr;
	}

	UClass* ParentClass = OriginalClass->GetSuperClass();
	if (!ParentClass)
	{
		ParentClass = AActor::StaticClass();
		Warn(TEXT("Could not determine parent class, defaulting to AActor"));
	}

	FString BPName = ExistingBPGC->GetName();
	if (BPName.EndsWith(TEXT("_C")))
	{
		BPName.LeftChopInline(2);
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Live uncooking '%s' — modifying existing BPGC in place"),
		*BPName);

	// Create transient UBlueprint that targets the existing cooked BPGC.
	// Name MUST match the BPGC name (minus _C suffix) so the compiler's
	// GetBlueprintClassNames() computes the same class name as the existing BPGC.
	// e.g., BP named "MainGamePC" → compiler expects "MainGamePC_C" → matches ExistingBPGC.
	UPackage* TransientPkg = GetTransientPackage();
	UBlueprint* BP = NewObject<UBlueprint>(TransientPkg, FName(*BPName));
	BP->ParentClass = ParentClass;
	BP->GeneratedClass = ExistingBPGC;
	BP->SkeletonGeneratedClass = ExistingBPGC;
	BP->BlueprintType = BPTYPE_Normal;
	ExistingBPGC->ClassGeneratedBy = BP;

	// Create event graph (cooked BPGCs don't have editor graphs)
	UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(BP,
		UEdGraphSchema_K2::GN_EventGraph,
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	BP->UbergraphPages.Add(EventGraph);

	if (!BuildBlueprintCore(BP, OriginalClass, Functions, /*bIsLiveUncook=*/ true))
	{
		return nullptr;
	}

	// Move interface function graphs from BP->FunctionGraphs into the matching
	// InterfaceDesc.Graphs entries. During compilation, ConformImplementedInterfaces
	// calls ConformInterfaceByName which checks InterfaceDesc.Graphs to see if
	// a graph already exists for each interface function. If not found there, it
	// calls CreateNewGraph — which asserts because a graph with that name already
	// exists in BP->FunctionGraphs (created by BuildFunctionGraph).
	// Moving them into InterfaceDesc.Graphs prevents the collision.
	for (FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface) continue;

		for (TFieldIterator<UFunction> FuncIt(InterfaceDesc.Interface); FuncIt; ++FuncIt)
		{
			FName FuncName = FuncIt->GetFName();

			for (int32 i = BP->FunctionGraphs.Num() - 1; i >= 0; --i)
			{
				if (BP->FunctionGraphs[i]->GetFName() == FuncName)
				{
					InterfaceDesc.Graphs.Add(BP->FunctionGraphs[i]);
					BP->FunctionGraphs.RemoveAt(i);
					break;
				}
			}
		}
	}

	return BP;
}

UBlueprint* FGraphBuilder::BuildChildBlueprint(
	UClass* OriginalClass,
	const TArray<FDecompiledFunction>& Functions,
	UPackage* TargetPackage,
	const FString& BlueprintName)
{
	if (!OriginalClass || !TargetPackage)
	{
		Warn(TEXT("Null class or package for BuildChildBlueprint"));
		return nullptr;
	}

	// ParentClass = the cooked class itself. The child IS-A cooked class,
	// so Cast<CookedClass> succeeds on instances of the child.
	UE_LOG(LogGraphBuilder, Log, TEXT("Creating child Blueprint '%s' inheriting from '%s'"),
		*BlueprintName, *OriginalClass->GetName());

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		OriginalClass,  // Parent IS the cooked class
		TargetPackage,
		FName(*BlueprintName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		Warn(TEXT("FKismetEditorUtilities::CreateBlueprint returned null for child"));
		return nullptr;
	}

	// Tell the event graph builder that all events are overrides of the parent
	bBuildingChildClass = true;

	if (!BuildBlueprintCore(NewBP, OriginalClass, Functions, /*bIsLiveUncook=*/ false, /*bIsChildClass=*/ true))
	{
		bBuildingChildClass = false;
		return nullptr;
	}
	bBuildingChildClass = false;

	// Move interface function graphs from BP->FunctionGraphs into the matching
	// InterfaceDesc.Graphs entries. During compilation, ConformImplementedInterfaces
	// calls ConformInterfaceByName which checks InterfaceDesc.Graphs to see if
	// a graph already exists for each interface function. If not found there, it
	// calls CreateNewGraph — which asserts because a graph with that name already
	// exists in BP->FunctionGraphs (created by BuildFunctionGraph).
	for (FBPInterfaceDescription& InterfaceDesc : NewBP->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface) continue;

		for (TFieldIterator<UFunction> FuncIt(InterfaceDesc.Interface); FuncIt; ++FuncIt)
		{
			FName FuncName = FuncIt->GetFName();

			for (int32 i = NewBP->FunctionGraphs.Num() - 1; i >= 0; --i)
			{
				if (NewBP->FunctionGraphs[i]->GetFName() == FuncName)
				{
					InterfaceDesc.Graphs.Add(NewBP->FunctionGraphs[i]);
					NewBP->FunctionGraphs.RemoveAt(i);
					break;
				}
			}
		}
	}

	return NewBP;
}

bool FGraphBuilder::BuildBlueprintCore(
	UBlueprint* NewBP,
	UClass* OriginalClass,
	const TArray<FDecompiledFunction>& Functions,
	bool bIsLiveUncook,
	bool bIsChildClass)
{
	// Store for use by CreateNodeForExpr virtual function resolution
	CurrentOriginalClass = OriginalClass;

	if (bIsChildClass)
	{
		// Child class mode: variables, components, and CDO defaults are all
		// inherited from the cooked parent class. Only interfaces need setup
		// so interface function graphs compile correctly as override implementations.
		SetupInterfaces(NewBP, OriginalClass);

		// Force skeleton class regeneration. The child's skeleton class inherits
		// parent properties, so AllocateDefaultPins() will find them.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
	}
	else
	{
		// Normal uncook: setup everything from the original class
		SetupVariables(NewBP, OriginalClass);
		SetupInterfaces(NewBP, OriginalClass);
		SetupComponents(NewBP, OriginalClass);

		if (!bIsLiveUncook)
		{
			// Copy CDO default property values.
			// Skipped for live uncook — the existing cooked CDO already has correct values.
			SetupCDODefaults(NewBP, OriginalClass);

			// Force skeleton class regeneration so that AllocateDefaultPins() can find
			// BP-defined properties when creating VariableGet/Set nodes. Without this,
			// variables defined on this class (not inherited) produce 0-pin nodes because
			// GetPropertyForVariable() searches the skeleton class which hasn't been updated yet.
			// Skipped for live uncook — SkeletonGeneratedClass is set to the existing BPGC
			// which already has all properties, so AllocateDefaultPins() works without regen.
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
		}
	}

	// Find the event graph (created by default by CreateBlueprint)
	UEdGraph* EventGraph = nullptr;
	if (NewBP->UbergraphPages.Num() > 0)
	{
		EventGraph = NewBP->UbergraphPages[0];
	}
	else
	{
		EventGraph = FBlueprintEditorUtils::CreateNewGraph(NewBP,
			UEdGraphSchema_K2::GN_EventGraph,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		NewBP->UbergraphPages.Add(EventGraph);
	}

	// Separate functions by type
	const FDecompiledFunction* UbergraphFunc = nullptr;
	TArray<FDecompiledFunction> EventStubs;
	TArray<FDecompiledFunction> RegularFunctions;

	for (const FDecompiledFunction& Func : Functions)
	{
		if (Func.bIsUbergraph)
		{
			UbergraphFunc = &Func;
		}
		else if (Func.bIsEventStub)
		{
			EventStubs.Add(Func);
		}
		else
		{
			RegularFunctions.Add(Func);
		}
	}

	// Build event graph from ubergraph
	if (UbergraphFunc)
	{
		BuildEventGraph(NewBP, EventGraph, *UbergraphFunc, EventStubs, OriginalClass);
	}

	// Build function graphs for regular functions
	// First, collect event stub parameter names — these are stored to the persistent
	// frame by event stubs and accessed by implementation functions via EX_LocalOutVariable.
	// Functions that are called from the ubergraph with persistent frame args need
	// these added as parameters even though the function UFunction lacks CPF_Parm.
	EventStubParamNames.Reset();
	for (const FDecompiledFunction& Stub : EventStubs)
	{
		if (Stub.OriginalFunction)
		{
			for (TFieldIterator<FProperty> It(Stub.OriginalFunction); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					EventStubParamNames.Add(It->GetFName());
				}
			}
		}
	}

	for (const FDecompiledFunction& Func : RegularFunctions)
	{
		if (Func.Statements.Num() > 0)
		{
			BuildFunctionGraph(NewBP, Func, OriginalClass);
		}
	}

	// Post-build: Generate skeleton and refresh all nodes
	// GenerateBlueprintSkeleton reads function entry/result nodes from the
	// graphs and creates UFunction stubs on the skeleton class.  This is
	// independent of BP->ParentClass — stubs are built from OUR graphs, not
	// inherited.  Once the skeleton has stubs, RefreshAllNodes can safely
	// call ReconstructNode → AllocateDefaultPins → GetTargetFunction →
	// ResolveMember and find every self-member function.
	//
	// The UE4 compilation pipeline (BlueprintCompilationManager STAGE VIII/IX)
	// generates the skeleton before reconstructing nodes, but STAGE IX only
	// calls OptionallyRefreshNodes (a no-op outside hot-reload).  Running
	// RefreshAllNodes HERE ensures that all pin types, defaults, and wiring
	// are cleaned up before compilation — matching what the BP editor does
	// after every user edit.
	{
		FKismetEditorUtilities::GenerateBlueprintSkeleton(NewBP, /*bForceRegeneration=*/ true);
		FBlueprintEditorUtils::RefreshAllNodes(NewBP);
	}

	// Post-refresh: Propagate array wildcard types
	// RefreshAllNodes calls ReconstructNode which restores connections but
	// does NOT call PinConnectionListChanged. Array function nodes (using
	// UK2Node_CallArrayFunction) rely on PinConnectionListChanged to propagate
	// element types from connected pins. Without this pass, Array_Get/Length/etc.
	// keep wildcard pins even when connected to typed arrays.
	{
		TArray<UEdGraph*> AllGraphs;
		AllGraphs.Append(NewBP->UbergraphPages);
		AllGraphs.Append(NewBP->FunctionGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				// Check if any pin is wildcard and connected to a typed pin
				bool bNeedsNotification = false;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->LinkedTo.Num() == 0) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					{
						// Find a typed connection to propagate from
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
							{
								Pin->PinType = LinkedPin->PinType;
								bNeedsNotification = true;
								break;
							}
						}
					}
				}

				// Notify the node so it can propagate types to other pins
				// (e.g., setting Array_Get's TargetArray type should update Item output)
				if (bNeedsNotification)
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin && Pin->LinkedTo.Num() > 0)
						{
							Node->PinConnectionListChanged(Pin);
						}
					}
				}
			}
		}
	}

	// Post-refresh: Fix Select node option pin types
	// UK2Node_Select option pins with literal defaults (no wire connections)
	// remain wildcard after reconstruction. The chain of failure is:
	//  1. RefreshAllNodes->ReconstructNode recreates all pins as wildcard
	//  2. PostReconstructNode fixes ReturnPin from its wire connection,
	//     but calls OnPinTypeChanged which sets bReconstructNode=true
	//     for a DEFERRED reconstruction that never fires during RefreshAllNodes
	//  3. The wildcard pass above fixes ReturnPin (already typed), but
	//     option pins have no connections and are skipped
	// Fix: directly propagate ReturnPin's resolved type to all wildcard
	// option pins after all other passes have run.
	{
		TArray<UEdGraph*> SelectGraphs;
		SelectGraphs.Append(NewBP->UbergraphPages);
		SelectGraphs.Append(NewBP->FunctionGraphs);

		for (UEdGraph* Graph : SelectGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node);
				if (!SelectNode) continue;

				UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
				if (!ReturnPin) continue;

				// Determine the resolved value type.
				// Priority: ReturnPin type (set by PostReconstructNode from wire),
				// then fall back to scanning ReturnPin's connections directly.
				FEdGraphPinType ResolvedType = ReturnPin->PinType;
				if (ResolvedType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					for (UEdGraphPin* LinkedPin : ReturnPin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
						{
							ResolvedType = LinkedPin->PinType;
							ReturnPin->PinType = ResolvedType;
							break;
						}
					}
				}

				// Also try option pin connections if ReturnPin is still wildcard
				if (ResolvedType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					TArray<UEdGraphPin*> OptionPins;
					SelectNode->GetOptionPins(OptionPins);
					for (UEdGraphPin* OptPin : OptionPins)
					{
						if (OptPin)
						{
							for (UEdGraphPin* LinkedPin : OptPin->LinkedTo)
							{
								if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
								{
									ResolvedType = LinkedPin->PinType;
									ReturnPin->PinType = ResolvedType;
									break;
								}
							}
						}
						if (ResolvedType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
							break;
					}
				}

				if (ResolvedType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					continue; // No type info available, skip

				// Propagate to all wildcard option pins
				TArray<UEdGraphPin*> OptionPins;
				SelectNode->GetOptionPins(OptionPins);
				for (UEdGraphPin* OptionPin : OptionPins)
				{
					if (OptionPin && OptionPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					{
						OptionPin->PinType = ResolvedType;
					}
				}
			}
		}
	}

	// Post-build: Sanitize graph connections
	// Remove direction-mismatched connections (e.g., two output pins linked
	// together) that corrupt the compiler's CreateExecutionSchedule topological
	// sort. When the sort fails with "site 1" error, it empties the execution
	// list entirely, causing EventEntryHandler::RegisterNet to never run and
	// ALL SetVariableOnPersistentFrame nodes to fail with ICE errors.
	{
		TArray<UEdGraph*> SanitizeGraphs;
		SanitizeGraphs.Append(NewBP->UbergraphPages);
		SanitizeGraphs.Append(NewBP->FunctionGraphs);

		for (UEdGraph* Graph : SanitizeGraphs)
		{
			SanitizeGraphConnections(Graph);
		}
	}

	// Post-refresh: Restore deferred pin defaults
	// RefreshAllNodes->ReconstructNode destroys and recreates all pins.
	// For byte/enum pins, "None" maps to NAME_None which the engine treats
	// as empty/unset, so these defaults get silently dropped. Re-apply them.
	{
		int32 RestoredCount = 0;
		for (const FDeferredPinDefault& Deferred : DeferredPinDefaults)
		{
			UEdGraphNode* Node = Deferred.Node.Get();
			if (!Node) continue;
			UEdGraphPin* Pin = Node->FindPin(Deferred.PinName);
			if (!Pin || Pin->LinkedTo.Num() > 0) continue;
			if (Pin->DefaultValue.IsEmpty() && !Deferred.DefaultValue.IsEmpty())
			{
				Pin->DefaultValue = Deferred.DefaultValue;
				RestoredCount++;
			}
			if (!Pin->DefaultObject && Deferred.DefaultObject.IsValid())
			{
				Pin->DefaultObject = Deferred.DefaultObject.Get();
				RestoredCount++;
			}
		}
		if (RestoredCount > 0)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("Restored %d deferred pin defaults after RefreshAllNodes"), RestoredCount);
		}
		DeferredPinDefaults.Reset();
	}

	// Post-refresh: Remove timeline-internal properties from NewVariables
	// Timeline templates (copied from BPGC or reconstructed) define their own
	// properties (Direction byte, float/vector track values) via
	// CreateClassVariablesFromBlueprint. If these same properties also exist
	// in BP->NewVariables (added by SetupVariables from the cooked class),
	// the compiler creates duplicates and renames the template versions to
	// _ERROR_DUPLICATE_0, breaking timeline data flow. Remove the NewVariables
	// entries AFTER RefreshAllNodes (which needs them on the skeleton class
	// for pin resolution) but BEFORE compilation.
	if (NewBP->Timelines.Num() > 0)
	{
		TSet<FName> TimelinePropertyNames;
		for (UTimelineTemplate* Timeline : NewBP->Timelines)
		{
			if (!Timeline) continue;

			// Direction property
			FName DirPropName = Timeline->GetDirectionPropertyName();
			if (DirPropName != NAME_None)
			{
				TimelinePropertyNames.Add(DirPropName);
			}

			// Float track properties
			for (const FTTFloatTrack& Track : Timeline->FloatTracks)
			{
				FName PropName = Track.GetPropertyName();
				if (PropName != NAME_None)
				{
					TimelinePropertyNames.Add(PropName);
				}
			}

			// Vector track properties
			for (const FTTVectorTrack& Track : Timeline->VectorTracks)
			{
				FName PropName = Track.GetPropertyName();
				if (PropName != NAME_None)
				{
					TimelinePropertyNames.Add(PropName);
				}
			}

			// Linear color track properties
			for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
			{
				FName PropName = Track.GetPropertyName();
				if (PropName != NAME_None)
				{
					TimelinePropertyNames.Add(PropName);
				}
			}
		}

		int32 RemovedCount = 0;
		for (int32 i = NewBP->NewVariables.Num() - 1; i >= 0; --i)
		{
			if (TimelinePropertyNames.Contains(NewBP->NewVariables[i].VarName))
			{
				UE_LOG(LogGraphBuilder, Log, TEXT("Removing timeline-internal property '%s' from NewVariables (will be created from template)"),
					*NewBP->NewVariables[i].VarName.ToString());
				NewBP->NewVariables.RemoveAt(i);
				RemovedCount++;
			}
		}

		if (RemovedCount > 0)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("Removed %d timeline-internal properties from NewVariables to prevent compiler duplicates"), RemovedCount);
		}
	}

	// Final structural modification to regenerate skeleton class with all
	// type information properly resolved after the refresh
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);

	return true;
}

// Two-pass compile helpers

static TArray<UEdGraph*> GatherAllGraphs(UBlueprint* BP)
{
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(BP->UbergraphPages);
	AllGraphs.Append(BP->FunctionGraphs);
	return AllGraphs;
}

TArray<FGraphBuilder::FPinSnapshot> FGraphBuilder::SnapshotAllConnections(UBlueprint* BP)
{
	TArray<FPinSnapshot> Snapshot;
	if (!BP) return Snapshot;

	TArray<UEdGraph*> AllGraphs = GatherAllGraphs(BP);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;

				// For CONNECTIONS: only snapshot output pins to avoid double-wiring.
				// Every link is bidirectional (A→B means B→A), so recording only
				// output→input captures each connection exactly once.
				// For DEFAULTS: snapshot input pins with defaults (no connections).
				bool bHasConnections = Pin->LinkedTo.Num() > 0;
				bool bHasDefaults = !Pin->DefaultValue.IsEmpty() ||
					Pin->DefaultObject != nullptr ||
					!Pin->DefaultTextValue.IsEmpty();

				if (bHasConnections && Pin->Direction != EGPD_Output)
					continue; // Skip input-side connection records
				if (!bHasConnections && !bHasDefaults)
					continue; // Skip empty pins

				FPinSnapshot Record;
				Record.OwningNodeGuid = Node->NodeGuid;
				Record.PinName = Pin->GetFName();
				Record.Direction = Pin->Direction;
				Record.DefaultValue = Pin->DefaultValue;
				Record.DefaultObject = Pin->DefaultObject;
				Record.DefaultTextValue = Pin->DefaultTextValue;

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						Record.LinkedPinIds.Emplace(
							LinkedPin->GetOwningNode()->NodeGuid,
							LinkedPin->GetFName());
					}
				}

				if (Record.LinkedPinIds.Num() > 0 || bHasDefaults)
				{
					Snapshot.Add(MoveTemp(Record));
				}
			}
		}
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Snapshot captured %d pin records"), Snapshot.Num());
	return Snapshot;
}

void FGraphBuilder::RestoreConnectionsFromSnapshot(UBlueprint* BP, const TArray<FPinSnapshot>& Snapshot)
{
	if (!BP || Snapshot.Num() == 0) return;

	// Build node lookup by NodeGuid
	TMap<FGuid, UEdGraphNode*> NodeLookup;
	TArray<UEdGraph*> AllGraphs = GatherAllGraphs(BP);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				NodeLookup.Add(Node->NodeGuid, Node);
			}
		}
	}

	int32 RestoredConnections = 0;
	int32 RestoredDefaults = 0;
	int32 FailedConnections = 0;

	for (const FPinSnapshot& Record : Snapshot)
	{
		UEdGraphNode** OwnerNodePtr = NodeLookup.Find(Record.OwningNodeGuid);
		if (!OwnerNodePtr || !*OwnerNodePtr) continue;

		UEdGraphPin* SourcePin = (*OwnerNodePtr)->FindPin(Record.PinName, Record.Direction);
		if (!SourcePin)
		{
			FailedConnections += Record.LinkedPinIds.Num();
			continue;
		}

		// Restore connections
		for (const auto& LinkedPinInfo : Record.LinkedPinIds)
		{
			UEdGraphNode** TargetNodePtr = NodeLookup.Find(LinkedPinInfo.Key);
			if (!TargetNodePtr || !*TargetNodePtr)
			{
				FailedConnections++;
				continue;
			}

			// Find target pin with opposite direction
			EEdGraphPinDirection TargetDir =
				(Record.Direction == EGPD_Input) ? EGPD_Output : EGPD_Input;
			UEdGraphPin* TargetPin = (*TargetNodePtr)->FindPin(LinkedPinInfo.Value, TargetDir);
			if (!TargetPin)
			{
				// Fallback: try any direction
				TargetPin = (*TargetNodePtr)->FindPin(LinkedPinInfo.Value);
			}

			if (!TargetPin)
			{
				FailedConnections++;
				continue;
			}

			if (!SourcePin->LinkedTo.Contains(TargetPin))
			{
				SourcePin->MakeLinkTo(TargetPin);
				RestoredConnections++;
			}
		}

		// Restore defaults if they were cleared by ReconstructNode
		if (SourcePin->LinkedTo.Num() == 0)
		{
			if (SourcePin->DefaultValue.IsEmpty() && !Record.DefaultValue.IsEmpty())
			{
				SourcePin->DefaultValue = Record.DefaultValue;
				RestoredDefaults++;
			}
			if (!SourcePin->DefaultObject && Record.DefaultObject.IsValid())
			{
				SourcePin->DefaultObject = Record.DefaultObject.Get();
				RestoredDefaults++;
			}
			if (SourcePin->DefaultTextValue.IsEmpty() && !Record.DefaultTextValue.IsEmpty())
			{
				SourcePin->DefaultTextValue = Record.DefaultTextValue;
				RestoredDefaults++;
			}
		}
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Two-pass restore: %d connections restored, %d defaults restored, %d connections failed"),
		RestoredConnections, RestoredDefaults, FailedConnections);
}

void FGraphBuilder::RunPostRefreshFixups(UBlueprint* BP)
{
	if (!BP) return;

	TArray<UEdGraph*> AllGraphs = GatherAllGraphs(BP);

	// --- Wildcard type propagation ---
	// Array function nodes need type propagation from connected pins
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			bool bNeedsNotification = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->LinkedTo.Num() == 0) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
						{
							Pin->PinType = LinkedPin->PinType;
							bNeedsNotification = true;
							break;
						}
					}
				}
			}

			if (bNeedsNotification)
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->LinkedTo.Num() > 0)
					{
						Node->PinConnectionListChanged(Pin);
					}
				}
			}
		}
	}

	// --- Select node option pin type fix ---
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node);
			if (!SelectNode) continue;

			UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
			if (!ReturnPin) continue;

			FEdGraphPinType ResolvedType = ReturnPin->PinType;
			if (ResolvedType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				for (UEdGraphPin* LinkedPin : ReturnPin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
					{
						ResolvedType = LinkedPin->PinType;
						ReturnPin->PinType = ResolvedType;
						break;
					}
				}
			}

			if (ResolvedType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				TArray<UEdGraphPin*> OptionPins;
				SelectNode->GetOptionPins(OptionPins);
				for (UEdGraphPin* OptPin : OptionPins)
				{
					if (OptPin)
					{
						for (UEdGraphPin* LinkedPin : OptPin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
							{
								ResolvedType = LinkedPin->PinType;
								ReturnPin->PinType = ResolvedType;
								break;
							}
						}
					}
					if (ResolvedType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
						break;
				}
			}

			if (ResolvedType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				continue;

			TArray<UEdGraphPin*> OptionPins;
			SelectNode->GetOptionPins(OptionPins);
			for (UEdGraphPin* OptionPin : OptionPins)
			{
				if (OptionPin && OptionPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					OptionPin->PinType = ResolvedType;
				}
			}
		}
	}

	// --- Sanitize invalid connections ---
	for (UEdGraph* Graph : AllGraphs)
	{
		SanitizeGraphConnections(Graph);
	}
}

// Event Reference Snapshot/Restore

TArray<FGraphBuilder::FEventRefSnapshot> FGraphBuilder::SnapshotEventReferences(UBlueprint* BP)
{
	TArray<FEventRefSnapshot> Snapshot;
	if (!BP) return Snapshot;

	TArray<UEdGraph*> AllGraphs = GatherAllGraphs(BP);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
			if (!EventNode) continue;

			FEventRefSnapshot Record;
			Record.NodeGuid = EventNode->NodeGuid;
			Record.EventName = EventNode->EventReference.GetMemberName();

			// Get the declaring class from the current EventReference
			UClass* MemberClass = EventNode->EventReference.GetMemberParentClass();
			if (MemberClass)
			{
				Record.DeclaringClass = MemberClass;
				Snapshot.Add(Record);
			}
		}
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Snapshot captured %d event references"), Snapshot.Num());
	return Snapshot;
}

void FGraphBuilder::RestoreEventReferences(UBlueprint* BP, const TArray<FEventRefSnapshot>& Snapshot)
{
	if (!BP || Snapshot.Num() == 0) return;

	// Build lookup by NodeGuid
	TMap<FGuid, const FEventRefSnapshot*> SnapshotLookup;
	for (const FEventRefSnapshot& Record : Snapshot)
	{
		SnapshotLookup.Add(Record.NodeGuid, &Record);
	}

	TArray<UEdGraph*> AllGraphs = GatherAllGraphs(BP);
	int32 RestoredCount = 0;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
			if (!EventNode) continue;

			const FEventRefSnapshot** RecordPtr = SnapshotLookup.Find(EventNode->NodeGuid);
			if (!RecordPtr || !*RecordPtr) continue;

			const FEventRefSnapshot& Record = **RecordPtr;
			UClass* OrigDeclaringClass = Record.DeclaringClass.Get();
			if (!OrigDeclaringClass) continue;

			// Check if FixupEventReference corrupted the declaring class
			UClass* CurrentClass = EventNode->EventReference.GetMemberParentClass();
			if (CurrentClass != OrigDeclaringClass)
			{
				EventNode->EventReference.SetExternalMember(Record.EventName, OrigDeclaringClass);
				RestoredCount++;
			}
		}
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Restored %d event references after RefreshAllNodes"), RestoredCount);
}

// Variable Setup

void FGraphBuilder::SetupVariables(UBlueprint* BP, UClass* OriginalClass)
{
	// Copy variable definitions from the original class's properties
	for (TFieldIterator<FProperty> It(OriginalClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Prop = *It;

		// Skip properties that belong to the parent
		if (Prop->GetOwnerClass() != OriginalClass)
			continue;

		// Skip internal/generated properties
		if (Prop->HasAnyPropertyFlags(CPF_Parm | CPF_ReturnParm))
			continue;

		// Skip UTimelineComponent properties — these are owned by UK2Node_Timeline
		// nodes which create the actual component with curve data during compilation.
		// Including them here creates a duplicate variable (plain, no curves) that
		// shadows the real one, so calls like Play()/Reverse() do nothing.
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UTimelineComponent::StaticClass()))
			{
				UE_LOG(LogGraphBuilder, Verbose, TEXT("Skipping timeline component variable: %s (owned by UK2Node_Timeline)"),
					*Prop->GetName());
				continue;
			}
		}

		// Check if this variable already exists
		bool bAlreadyExists = false;
		for (const FBPVariableDescription& ExistingVar : BP->NewVariables)
		{
			if (ExistingVar.VarName == Prop->GetFName())
			{
				bAlreadyExists = true;
				break;
			}
		}
		if (bAlreadyExists) continue;

		FBPVariableDescription NewVar;
		NewVar.VarName = Prop->GetFName();
		NewVar.VarGuid = FGuid::NewGuid();
		NewVar.FriendlyName = Prop->GetName();
		NewVar.PropertyFlags = Prop->PropertyFlags;
		NewVar.RepNotifyFunc = NAME_None;

		// Derive FEdGraphPinType from the FProperty
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Schema->ConvertPropertyToPinType(Prop, NewVar.VarType);

		// Try to set category from metadata
		if (Prop->HasMetaData(TEXT("Category")))
		{
			NewVar.Category = FText::FromString(Prop->GetMetaData(TEXT("Category")));
		}

		BP->NewVariables.Add(NewVar);

		UE_LOG(LogGraphBuilder, Verbose, TEXT("Added variable: %s (%s)"),
			*NewVar.VarName.ToString(), *NewVar.VarType.PinCategory.ToString());
	}

	// NOTE: We intentionally do NOT walk up the parent class chain to add
	// inherited BPGC properties. Even though the parent is a cooked BPGC,
	// its FProperties still exist on the loaded UClass and are inherited
	// normally through the class hierarchy. Adding them as NewVariables
	// here causes the BP compiler to create duplicate FProperties that
	// collide with the parent's (e.g., "Tried to create property X in
	// scope SKEL_Gregory_C but FirstPersonCharacter_C:X already exists").
	// VariableGet/Set nodes using SetSelfMember will resolve through
	// normal class hierarchy lookup at compile time.
}

void FGraphBuilder::SetupInterfaces(UBlueprint* BP, UClass* OriginalClass)
{
	// Copy implemented interfaces
	for (const FImplementedInterface& Interface : OriginalClass->Interfaces)
	{
		if (Interface.Class)
		{
			FBPInterfaceDescription NewInterface;
			NewInterface.Interface = Interface.Class;
			BP->ImplementedInterfaces.Add(NewInterface);

			UE_LOG(LogGraphBuilder, Verbose, TEXT("Added interface: %s"),
				*Interface.Class->GetName());
		}
	}
}

// Component Setup - Copies SCS and component templates from original class

void FGraphBuilder::SetupComponents(UBlueprint* BP, UClass* OriginalClass)
{
	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(OriginalClass);
	if (!BPGC) return;

	// ----------------------------------------------------------------
	// Approach 1: Clone the SimpleConstructionScript if it exists
	// ----------------------------------------------------------------
	USimpleConstructionScript* OriginalSCS = BPGC->SimpleConstructionScript;
	if (OriginalSCS)
	{
		UE_LOG(LogGraphBuilder, Log, TEXT("Cloning SimpleConstructionScript (%d root nodes)"),
			OriginalSCS->GetRootNodes().Num());

		// Duplicate the entire SCS into the new Blueprint's GeneratedClass.
		// The engine requires SCS->GetOuter() == GeneratedClass (validated in
		// UBlueprint::ValidateGeneratedClass at Blueprint.cpp:1258).
		UObject* SCSOwner = BP->GeneratedClass ? (UObject*)BP->GeneratedClass : (UObject*)BP;
		USimpleConstructionScript* NewSCS = DuplicateObject<USimpleConstructionScript>(OriginalSCS, SCSOwner);
		if (NewSCS)
		{
			BP->SimpleConstructionScript = NewSCS;
			// Mirror into BPGC — the compiler does this at KismetCompiler.cpp:2566,
			// but ValidateGeneratedClass checks it before compilation finishes.
			if (UBlueprintGeneratedClass* NewBPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass))
			{
				NewBPGC->SimpleConstructionScript = NewSCS;
			}

			// Re-parent all SCS node component templates to the GeneratedClass.
			// NOTE: We do NOT add SCS components to NewVariables because the
			// BP compiler creates properties from BOTH NewVariables AND SCS nodes.
			// Adding them to NewVariables would cause "already exists" collisions.
			// VariableGet nodes for SCS components resolve through the class
			// hierarchy during compilation (parent BPGC still has the properties).
			const TArray<USCS_Node*>& AllNodes = NewSCS->GetAllNodes();
			for (USCS_Node* Node : AllNodes)
			{
				if (Node && Node->ComponentTemplate)
				{
					// Rename the template into the GeneratedClass
					Node->ComponentTemplate->Rename(nullptr, SCSOwner);

					UE_LOG(LogGraphBuilder, Log, TEXT("  Component: %s (%s)"),
						*Node->GetVariableName().ToString(),
						*Node->ComponentTemplate->GetClass()->GetName());
				}
			}

			UE_LOG(LogGraphBuilder, Log, TEXT("SCS cloned with %d total nodes"), AllNodes.Num());
		}
		else
		{
			Warn(TEXT("Failed to duplicate SimpleConstructionScript"));
		}
	}
	else
	{
		UE_LOG(LogGraphBuilder, Log, TEXT("No SimpleConstructionScript found on original class"));

		// ----------------------------------------------------------------
		// Approach 2: Reconstruct SCS from CDO's subobjects
		// If there's no SCS (fully cooked), we can try to reconstruct from
		// the CDO's instanced subobjects
		// ----------------------------------------------------------------
		UObject* OriginalCDO = OriginalClass->GetDefaultObject();
		if (!OriginalCDO) return;

		// Create a fresh SCS with GeneratedClass as Outer (engine requirement)
		UObject* SCSOwner = BP->GeneratedClass ? (UObject*)BP->GeneratedClass : (UObject*)BP;
		USimpleConstructionScript* NewSCS = NewObject<USimpleConstructionScript>(SCSOwner);
		BP->SimpleConstructionScript = NewSCS;
		if (UBlueprintGeneratedClass* NewBPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass))
		{
			NewBPGC->SimpleConstructionScript = NewSCS;
		}

		// Find all component subobjects on the CDO
		TArray<UObject*> Subobjects;
		OriginalCDO->GetDefaultSubobjects(Subobjects);

		int32 ComponentCount = 0;
		for (UObject* Subobj : Subobjects)
		{
			UActorComponent* CompTemplate = Cast<UActorComponent>(Subobj);
			if (!CompTemplate) continue;

			// Skip components that belong to the parent class (inherited)
			// We only want components added by THIS blueprint
			UObject* Archetype = CompTemplate->GetArchetype();
			if (Archetype && Archetype != CompTemplate &&
				Archetype->GetOuter() != OriginalCDO)
			{
				continue;
			}

			// Duplicate the component template
			UActorComponent* NewTemplate = DuplicateObject<UActorComponent>(CompTemplate, BP);
			if (!NewTemplate) continue;

			// Create an SCS node for this component
			USCS_Node* NewNode = NewSCS->CreateNode(NewTemplate->GetClass(), CompTemplate->GetFName());
			if (NewNode)
			{
				// Replace the auto-created template with our duplicated one
				if (NewNode->ComponentTemplate)
				{
					NewNode->ComponentTemplate->MarkPendingKill();
				}
				NewNode->ComponentTemplate = NewTemplate;

				// Add as root node (we lose hierarchy info from cooked data)
				NewSCS->AddNode(NewNode);
				ComponentCount++;

				UE_LOG(LogGraphBuilder, Log, TEXT("  Reconstructed component: %s (%s)"),
					*CompTemplate->GetFName().ToString(),
					*CompTemplate->GetClass()->GetName());
			}
		}

		if (ComponentCount > 0)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("Reconstructed %d components from CDO subobjects"), ComponentCount);
		}
	}

	// ----------------------------------------------------------------
	// Copy component class templates (used by AddComponent nodes etc.)
	// ----------------------------------------------------------------
	if (BPGC->ComponentTemplates.Num() > 0)
	{
		UE_LOG(LogGraphBuilder, Log, TEXT("Copying %d component templates"), BPGC->ComponentTemplates.Num());

		for (UActorComponent* Template : BPGC->ComponentTemplates)
		{
			if (!Template) continue;

			UActorComponent* NewTemplate = DuplicateObject<UActorComponent>(Template, BP);
			if (NewTemplate)
			{
				BP->ComponentTemplates.Add(NewTemplate);
			}
		}
	}

	// ----------------------------------------------------------------
	// Copy Timelines if present
	// ----------------------------------------------------------------
	UE_LOG(LogGraphBuilder, Log, TEXT("BPGC->Timelines count: %d"), BPGC->Timelines.Num());
	if (BPGC->Timelines.Num() > 0)
	{
		UE_LOG(LogGraphBuilder, Log, TEXT("Copying %d timeline templates"), BPGC->Timelines.Num());

		for (UTimelineTemplate* Timeline : BPGC->Timelines)
		{
			if (!Timeline) continue;

			UE_LOG(LogGraphBuilder, Log, TEXT("  Template '%s': VariableName='%s', FloatTracks=%d, VectorTracks=%d, GUID=%s"),
				*Timeline->GetName(),
				*Timeline->GetVariableName().ToString(),
				Timeline->FloatTracks.Num(),
				Timeline->VectorTracks.Num(),
				*Timeline->TimelineGuid.ToString());

			// Use GeneratedClass as outer (not BP itself) — engine's FindTimelineTemplateByVariableName
			// asserts that Timeline->GetOuter()->IsA(UClass::StaticClass())
			UObject* TimelineOuter = BP->GeneratedClass ? (UObject*)BP->GeneratedClass : (UObject*)BP;

			// Save original GUID before duplication — PostDuplicate() generates a NEW random GUID
			// and recomputes all GUID-dependent property names (DirectionPropertyName, track
			// PropertyNames). We must restore the original GUID because our NewVariables and
			// graph nodes reference property names computed from it.
			FGuid OriginalGuid = Timeline->TimelineGuid;

			UTimelineTemplate* NewTimeline = DuplicateObject<UTimelineTemplate>(Timeline, TimelineOuter);
			if (NewTimeline)
			{
				// Restore original GUID and re-cache all property names to match.
				// PostDuplicate generated a new GUID and recomputed names with it.
				// We can't call UpdateCachedNames() directly (it's private), and
				// PostEditChange() doesn't trigger it (no PostEditChangeProperty override).
				// PostEditImport() DOES call UpdateCachedNames() without generating a new GUID.
				NewTimeline->TimelineGuid = OriginalGuid;
				NewTimeline->PostEditImport(); // calls UpdateCachedNames() to recompute with original GUID

				BP->Timelines.Add(NewTimeline);

				// Log with verification of property names
				UE_LOG(LogGraphBuilder, Log, TEXT("  -> Copied as '%s' (outer: %s, GUID preserved: %s)"),
					*NewTimeline->GetName(), *TimelineOuter->GetName(), *OriginalGuid.ToString());
				UE_LOG(LogGraphBuilder, Log, TEXT("     VariableName='%s', DirectionProp='%s'"),
					*NewTimeline->GetVariableName().ToString(),
					*NewTimeline->GetDirectionPropertyName().ToString());
				for (int32 TrackIdx = 0; TrackIdx < NewTimeline->FloatTracks.Num(); ++TrackIdx)
				{
					const FTTFloatTrack& Track = NewTimeline->FloatTracks[TrackIdx];
					UE_LOG(LogGraphBuilder, Log, TEXT("     FloatTrack[%d]: TrackName='%s', PropertyName='%s', CurveFloat=%s"),
						TrackIdx, *Track.GetTrackName().ToString(), *Track.GetPropertyName().ToString(),
						Track.CurveFloat ? *Track.CurveFloat->GetPathName() : TEXT("NULL"));
				}
			}
		}
	}
	else
	{
		// Cooked BPGCs often have no timeline templates (stripped during cooking).
		// Try to reconstruct from timeline components on the CDO.
		UE_LOG(LogGraphBuilder, Log, TEXT("No timeline templates in BPGC — attempting reconstruction from cooked data"));

		UObject* CDO = OriginalClass->GetDefaultObject();
		for (TFieldIterator<FObjectProperty> It(OriginalClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FObjectProperty* ObjProp = *It;
			if (!ObjProp->PropertyClass || !ObjProp->PropertyClass->IsChildOf(UTimelineComponent::StaticClass()))
				continue;

			UTimelineComponent* TimelineComp = CDO ? Cast<UTimelineComponent>(ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO))) : nullptr;
			FName TimelineVarName = ObjProp->GetFName();
			FString SanitizedName = TimelineVarName.ToString();
			SanitizedName.ReplaceCharInline(TEXT(' '), TEXT('_'));
			SanitizedName.ReplaceCharInline(TEXT('/'), TEXT('_'));

			// Find the GUID from the Direction property: {SanitizedName}__Direction_{GUID}
			FString DirectionPrefix = SanitizedName + TEXT("__Direction_");
			FGuid ExtractedGuid;
			FName DirectionPropName = NAME_None;
			for (TFieldIterator<FProperty> PropIt(OriginalClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
			{
				FString PropName = PropIt->GetName();
				if (PropName.StartsWith(DirectionPrefix))
				{
					FString GuidStr = PropName.Mid(DirectionPrefix.Len());
					FGuid::Parse(GuidStr, ExtractedGuid);
					DirectionPropName = PropIt->GetFName();
					break;
				}
			}

			if (!ExtractedGuid.IsValid())
			{
				UE_LOG(LogGraphBuilder, Warning, TEXT("Could not extract GUID for timeline '%s' — skipping reconstruction"), *TimelineVarName.ToString());
				continue;
			}

			// Create the template
			FString TemplateName = UTimelineTemplate::TimelineVariableNameToTemplateName(TimelineVarName);
			UObject* TimelineOuter = BP->GeneratedClass ? (UObject*)BP->GeneratedClass : (UObject*)BP;
			UTimelineTemplate* NewTemplate = NewObject<UTimelineTemplate>(TimelineOuter, *TemplateName);
			NewTemplate->TimelineGuid = ExtractedGuid;
			NewTemplate->TimelineLength = TimelineComp ? TimelineComp->GetTimelineLength() : 1.0f;
			NewTemplate->bAutoPlay = false;
			NewTemplate->bLoop = TimelineComp ? TimelineComp->IsLooping() : false;

			// PostInitProperties (called by NewObject) already cached VariableName from the
			// object name, but DirectionPropertyName was computed with the default zero GUID.
			// Re-cache now with the correct GUID so SetTrackName (which reads VariableName)
			// works, and DirectionPropertyName gets the correct GUID suffix.
			// PostEditChange() does NOT call UpdateCachedNames on UTimelineTemplate,
			// but PostEditImport() does.
			NewTemplate->PostEditImport();

			// Find float track properties: {SanitizedName}_{TrackName}_{GUID}
			FString GuidStr = ExtractedGuid.ToString();
			FString TrackSuffix = TEXT("_") + GuidStr;
			FString TrackPrefix = SanitizedName + TEXT("_");
			for (TFieldIterator<FFloatProperty> FloatIt(OriginalClass, EFieldIteratorFlags::ExcludeSuper); FloatIt; ++FloatIt)
			{
				FString FloatPropName = FloatIt->GetName();
				if (FloatPropName.StartsWith(TrackPrefix) && FloatPropName.EndsWith(TrackSuffix) && FloatPropName != DirectionPropName.ToString())
				{
					// Extract track name: between prefix and suffix
					FString TrackName = FloatPropName.Mid(TrackPrefix.Len(), FloatPropName.Len() - TrackPrefix.Len() - TrackSuffix.Len());

					FTTFloatTrack NewTrack;
					NewTrack.SetTrackName(FName(*TrackName), NewTemplate);

					// Try to find the UCurveFloat* for this track via reflection on the
					// timeline component. FTimeline::InterpFloats is private with no public
					// getter, so we use FStructProperty reflection to reach into it.
					if (TimelineComp)
					{
						// Access TheTimeline struct via reflection
						FStructProperty* TimelineProp = CastField<FStructProperty>(
							UTimelineComponent::StaticClass()->FindPropertyByName(TEXT("TheTimeline")));
						if (TimelineProp)
						{
							void* TimelinePtr = TimelineProp->ContainerPtrToValuePtr<void>(TimelineComp);
							// Access InterpFloats array within FTimeline
							FArrayProperty* FloatsProp = CastField<FArrayProperty>(
								TimelineProp->Struct->FindPropertyByName(TEXT("InterpFloats")));
							if (FloatsProp && TimelinePtr)
							{
								FScriptArrayHelper ArrayHelper(FloatsProp, FloatsProp->ContainerPtrToValuePtr<void>(TimelinePtr));
								FStructProperty* ElemProp = CastField<FStructProperty>(FloatsProp->Inner);
								for (int32 Idx = 0; Idx < ArrayHelper.Num(); ++Idx)
								{
									void* ElemPtr = ArrayHelper.GetRawPtr(Idx);
									// Check TrackName field
									FNameProperty* TrackNameProp = CastField<FNameProperty>(
										ElemProp->Struct->FindPropertyByName(TEXT("TrackName")));
									if (TrackNameProp)
									{
										FName CompTrackName = TrackNameProp->GetPropertyValue(
											TrackNameProp->ContainerPtrToValuePtr<void>(ElemPtr));
										if (CompTrackName.ToString() == TrackName)
										{
											// Found matching track — get FloatCurve
											FObjectProperty* CurveProp = CastField<FObjectProperty>(
												ElemProp->Struct->FindPropertyByName(TEXT("FloatCurve")));
											if (CurveProp)
											{
												UCurveFloat* Curve = Cast<UCurveFloat>(
													CurveProp->GetObjectPropertyValue(
														CurveProp->ContainerPtrToValuePtr<void>(ElemPtr)));
												if (Curve)
												{
													NewTrack.CurveFloat = Curve;
													UE_LOG(LogGraphBuilder, Log, TEXT("    Found curve asset: %s"),
														*Curve->GetPathName());
												}
											}
											break;
										}
									}
								}
							}
						}
					}

					NewTemplate->FloatTracks.Add(NewTrack);
					UE_LOG(LogGraphBuilder, Log, TEXT("  Reconstructed float track: %s (property: %s)"),
						*TrackName, *FloatPropName);
				}
			}

			// Final re-cache after tracks were added — PostEditImport calls UpdateCachedNames
			NewTemplate->PostEditImport();

			BP->Timelines.Add(NewTemplate);
			UE_LOG(LogGraphBuilder, Log, TEXT("Reconstructed timeline template '%s' (GUID: %s, %d float tracks)"),
				*TemplateName, *GuidStr, NewTemplate->FloatTracks.Num());
		}
	}
}

// CDO Default Values - Copies default property values from original CDO

void FGraphBuilder::SetupCDODefaults(UBlueprint* BP, UClass* OriginalClass)
{
	UObject* OriginalCDO = OriginalClass->GetDefaultObject();
	if (!OriginalCDO) return;

	// We need to compile the BP first to generate a class with properties,
	// then copy defaults. But the class isn't generated yet at this point.
	// Instead, store the original CDO reference so we can copy after compilation.
	// For now, we copy what we can into the Blueprint's default object.

	// The new BP's generated class won't exist until compilation,
	// but we can try to pre-populate defaults via FBPVariableDescription
	for (FBPVariableDescription& Var : BP->NewVariables)
	{
		FProperty* OrigProp = OriginalClass->FindPropertyByName(Var.VarName);
		if (!OrigProp) continue;

		// Export the default value from the original CDO as a string
		FString DefaultValueStr;
		const void* PropAddr = OrigProp->ContainerPtrToValuePtr<void>(OriginalCDO);

		OrigProp->ExportTextItem(DefaultValueStr, PropAddr, nullptr, OriginalCDO, PPF_None);

		if (!DefaultValueStr.IsEmpty())
		{
			FStructProperty* StructProp = CastField<FStructProperty>(OrigProp);
			if (StructProp && StructProp->Struct)
			{
				FString StructName = StructProp->Struct->GetName();

				// FTransform exported with Quaternion rotation isn't parseable by BP variable defaults.
				// Skip these — they'll use the identity transform or whatever the compiled class picks up.
				if (StructName == TEXT("Transform") || StructName == TEXT("Quat"))
				{
					continue;  // Skip unparseable struct formats
				}

				// ExportTextItem produces named format (Pitch=X,Yaw=Y,Roll=Z) for Rotators,
				// but FBPVariableDescription::DefaultValue expects numeric: X,Y,Z
				if (StructName == TEXT("Rotator") && DefaultValueStr.Contains(TEXT("Pitch=")))
				{
					// FRotator::InitFromString only handles short-form P=/Y=/R=,
					// but ExportTextItem produces long-form Pitch=/Yaw=/Roll=
					float Pitch = 0, Yaw = 0, Roll = 0;
					FParse::Value(*DefaultValueStr, TEXT("Pitch="), Pitch);
					FParse::Value(*DefaultValueStr, TEXT("Yaw="), Yaw);
					FParse::Value(*DefaultValueStr, TEXT("Roll="), Roll);
					DefaultValueStr = FString::Printf(TEXT("%f,%f,%f"), Pitch, Yaw, Roll);
				}
				// Same for Vectors: (X=V,Y=V,Z=V) → V,V,V
				else if (StructName == TEXT("Vector") && DefaultValueStr.Contains(TEXT("X=")))
				{
					FVector Vec;
					if (Vec.InitFromString(DefaultValueStr))
					{
						DefaultValueStr = FString::Printf(TEXT("%f,%f,%f"), Vec.X, Vec.Y, Vec.Z);
					}
				}
			}

			Var.DefaultValue = DefaultValueStr;
		}
	}

	UE_LOG(LogGraphBuilder, Log, TEXT("Copied CDO default values for %d variables"), BP->NewVariables.Num());
}

// Event Graph Building

void FGraphBuilder::BuildEventGraph(
	UBlueprint* BP,
	UEdGraph* EventGraph,
	const FDecompiledFunction& UbergraphFunc,
	const TArray<FDecompiledFunction>& EventStubs,
	UClass* OriginalClass)
{
	UE_LOG(LogGraphBuilder, Log, TEXT("Building event graph (%d statements, %d events)"),
		UbergraphFunc.Statements.Num(), EventStubs.Num());

	// Clear persistent frame var map (accumulates across events within this event graph)
	PersistentFrameVarMap.Reset();

	// Track component bound events we've already created to deduplicate.
	// Some Blueprints have multiple BndEvt stubs for the same component+delegate pair.
	// At runtime, each creates a separate delegate binding, so ALL fire when the delegate
	// broadcasts. This causes conflicting actions (e.g., OpenDoor + CloseDoor both execute).
	// We only create the FIRST handler and skip subsequent duplicates.
	// Tracks how many BndEvt stubs have been assigned per component+signature combination.
	// When multiple delegate properties share the same signature type (e.g., OnOpenDoor and
	// OnCloseDoor both use FOnOpenCloseDoor), this counter ensures each BndEvt stub gets
	// assigned to a different delegate property in declaration order.
	TMap<FString, int32> BoundEventSignatureCounters;

	// Sort entry points by offset
	TArray<TPair<FName, int32>> SortedEntries;
	for (const auto& Entry : UbergraphFunc.EventEntryPoints)
	{
		SortedEntries.Add(TPair<FName, int32>(Entry.Key, Entry.Value));
	}
	SortedEntries.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B)
		{
			return A.Value < B.Value;
		});

	// ---- Build offset-to-statement-index map for control flow analysis ----
	TMap<int32, int32> OffsetToStmtIdx;
	for (int32 i = 0; i < UbergraphFunc.Statements.Num(); i++)
	{
		if (UbergraphFunc.Statements[i])
		{
			OffsetToStmtIdx.Add(UbergraphFunc.Statements[i]->StartOffset, i);
		}
	}

	// ---- Compute reachable statements for each event via BFS ----
	// Instead of naively assigning contiguous offset ranges (which causes code from
	// one event's jump targets to be misattributed to another event), we follow
	// control flow from each entry point to determine which statements are reachable.
	TMap<FName, TSet<int32>> EventReachableSets;

	for (const auto& Entry : SortedEntries)
	{
		const FName& EvtName = Entry.Key;
		int32 EvtEntryOffset = Entry.Value;

		TSet<int32> Reachable;
		TSet<int32> VisitedOffsets;
		TArray<int32> WorkList;

		int32* StartIdxPtr = OffsetToStmtIdx.Find(EvtEntryOffset);
		if (StartIdxPtr)
		{
			WorkList.Add(EvtEntryOffset);
			VisitedOffsets.Add(EvtEntryOffset);
		}

		while (WorkList.Num() > 0)
		{
			int32 CurrOffset = WorkList.Pop();
			int32* CurrIdxPtr = OffsetToStmtIdx.Find(CurrOffset);
			if (!CurrIdxPtr) continue;
			int32 CurrIdx = *CurrIdxPtr;
			if (CurrIdx < 0 || CurrIdx >= UbergraphFunc.Statements.Num()) continue;

			Reachable.Add(CurrIdx);
			const TSharedPtr<FDecompiledExpr>& Stmt = UbergraphFunc.Statements[CurrIdx];
			if (!Stmt) continue;

			// Helper to add a successor offset to the BFS work list
			auto AddSuccessor = [&](int32 Offset)
			{
				if (!VisitedOffsets.Contains(Offset))
				{
					VisitedOffsets.Add(Offset);
					WorkList.Add(Offset);
				}
			};

			// Helper to add the fall-through (next sequential statement)
			auto AddFallThrough = [&]()
			{
				int32 NextIdx = CurrIdx + 1;
				if (NextIdx < UbergraphFunc.Statements.Num() && UbergraphFunc.Statements[NextIdx])
				{
					AddSuccessor(UbergraphFunc.Statements[NextIdx]->StartOffset);
				}
			};

			switch (Stmt->Token)
			{
			case EX_Return:
			case EX_EndOfScript:
			case EX_PopExecutionFlow:
				// Terminal statements — no successors
				break;

			case EX_Jump:
				AddSuccessor(Stmt->JumpTarget);
				break;

			case EX_JumpIfNot:
				AddSuccessor(Stmt->JumpTarget);  // Explicit false-branch offset
				AddFallThrough();                // True/continue branch
				break;

			case EX_PopExecutionFlowIfNot:
				// True: condition is true, continue to next statement
				AddFallThrough();
				// False: pops execution flow stack to a previously pushed address.
				// That resume address is already captured as a successor when we
				// process the corresponding EX_PushExecutionFlow.
				// IMPORTANT: JumpTarget is NOT set for this opcode (defaults to 0),
				// so using it here would erroneously pull in the entire ubergraph
				// from offset 0, polluting this event with every other event's code.
				break;

			case EX_PushExecutionFlow:
				AddSuccessor(Stmt->JumpTarget);  // Resume address (reached via PopExecFlow)
				AddFallThrough();                // Continue execution
				break;

			case EX_SwitchValue:
				AddSuccessor(Stmt->JumpTarget);  // End-of-switch offset
				break;

			default:
				// All other statements: fall through to next
				AddFallThrough();

				// Check for latent function calls — add resume offset as successor.
				// Latent functions (Delay, MoveComponentTo, Timeline, etc.) store a
				// resume offset in their LatentActionInfo struct argument (via
				// EX_SkipOffsetConst). This offset is a separate entry point into the
				// ubergraph when the latent action completes. It MUST be included in
				// this event's reachable set, otherwise the post-latent continuation
				// code is excluded and the Delay's "Completed" pin has nowhere to wire.
				for (const TSharedPtr<FDecompiledExpr>& Child : Stmt->Children)
				{
					if (Child && Child->Token == EX_StructConst && Child->StructRef &&
						Child->StructRef->GetName() == TEXT("LatentActionInfo"))
					{
						for (const TSharedPtr<FDecompiledExpr>& StructChild : Child->Children)
						{
							if (StructChild && StructChild->Token == EX_SkipOffsetConst && StructChild->JumpTarget > 0)
							{
								AddSuccessor(StructChild->JumpTarget);
								break;
							}
						}
						break;
					}
				}
				break;
			}
		}

		EventReachableSets.Add(EvtName, MoveTemp(Reachable));

		UE_LOG(LogGraphBuilder, Log, TEXT("Event '%s' at offset 0x%04X: %d reachable statements"),
			*EvtName.ToString(), EvtEntryOffset, EventReachableSets[EvtName].Num());
	}

	int32 EventY = 0;

	// Track timeline nodes so UpdateFunc and FinishedFunc stubs merge into one UK2Node_Timeline.
	// Maps timeline variable name (e.g. "Timeline_0") → created node.
	TMap<FString, UK2Node_Timeline*> TimelineNodes;

	// Track input action/key events for Pressed/Released pair detection.
	// Maps "ActionName_Action" or "KeyName_Key" → last seen stub index.
	// Consecutive stub indices for the same action/key indicate a Pressed/Released pair.
	TMap<FString, int32> LastInputEventStubIndex;

	for (int32 EventIdx = 0; EventIdx < SortedEntries.Num(); EventIdx++)
	{
		const FName& EventName = SortedEntries[EventIdx].Key;
		int32 EntryOffset = SortedEntries[EventIdx].Value;

		// Get the reachable statement set for this event
		const TSet<int32>* pReachable = EventReachableSets.Find(EventName);
		if (!pReachable || pReachable->Num() == 0)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("Event '%s' has no reachable statements at offset 0x%04X — skipping"),
				*EventName.ToString(), EntryOffset);
			continue;
		}

		// Compute the statement index range that covers all reachable statements
		int32 StartStatementIdx = UbergraphFunc.Statements.Num();
		int32 EndStatementIdx = 0;
		for (int32 StmtIdx : *pReachable)
		{
			StartStatementIdx = FMath::Min(StartStatementIdx, StmtIdx);
			EndStatementIdx = FMath::Max(EndStatementIdx, StmtIdx);
		}
		EndStatementIdx += 1; // Make it exclusive

		// Find the original event stub function for metadata
		UFunction* StubFunction = nullptr;
		for (const FDecompiledFunction& Stub : EventStubs)
		{
			if (Stub.OriginalFunction->GetFName() == EventName)
			{
				StubFunction = Stub.OriginalFunction;
				break;
			}
		}

		// NOTE: Interface event implementations (e.g., OnOverlappedDoor from Door Interactor)
		// are NOT skipped here. ConformImplementedInterfaces only creates empty stubs —
		// the actual implementation code lives in the ubergraph and must be decompiled.
		// The override event detection below correctly identifies interface events via
		// ParentFunc from the interface class and creates the proper UK2Node_Event.

		// Create the event node
		UEdGraphNode* EventNode = nullptr;
		UEdGraphPin* EventExecOut = nullptr;

		// ── Check for special input event types FIRST ──
		FString EventNameStr = EventName.ToString();
		bool bHandledAsInputEvent = false;

		if (EventNameStr.StartsWith(TEXT("InpActEvt_")))
		{
			// Input Action or Input Key event
			// Pattern: InpActEvt_<Name>_K2Node_InputActionEvent_<N>
			//      or: InpActEvt_<Name>_K2Node_InputKeyEvent_<N>
			FString Remainder = EventNameStr.Mid(10); // After "InpActEvt_"

			FString InputName;
			int32 StubIndex = 0;
			bool bIsKeyEvent = false;

			int32 SepIdx = Remainder.Find(TEXT("_K2Node_InputActionEvent_"));
			if (SepIdx != INDEX_NONE)
			{
				InputName = Remainder.Left(SepIdx);
				StubIndex = FCString::Atoi(*Remainder.Mid(SepIdx + 25));
			}
			else
			{
				SepIdx = Remainder.Find(TEXT("_K2Node_InputKeyEvent_"));
				if (SepIdx != INDEX_NONE)
				{
					InputName = Remainder.Left(SepIdx);
					StubIndex = FCString::Atoi(*Remainder.Mid(SepIdx + 22));
					bIsKeyEvent = true;
				}
			}

			if (!InputName.IsEmpty())
			{
				// Determine Pressed vs Released via consecutive-index pair detection
				EInputEvent InputEventType = IE_Pressed;
				FString TrackKey = InputName + (bIsKeyEvent ? TEXT("_Key") : TEXT("_Action"));
				if (int32* PrevIdx = LastInputEventStubIndex.Find(TrackKey))
				{
					if (StubIndex == *PrevIdx + 1)
					{
						InputEventType = IE_Released;
					}
				}
				LastInputEventStubIndex.FindOrAdd(TrackKey) = StubIndex;

				if (bIsKeyEvent)
				{
					// Create UK2Node_InputKey for individual key bindings
					UK2Node_InputKey* InputKeyNode = NewObject<UK2Node_InputKey>(EventGraph);
					InputKeyNode->CreateNewGuid();
					InputKeyNode->InputKey = FKey(*InputName);
					InputKeyNode->NodePosX = 0;
					InputKeyNode->NodePosY = EventY;
					InputKeyNode->AllocateDefaultPins();
					EventGraph->AddNode(InputKeyNode, false, false);
					EventNode = InputKeyNode;
				}
				else
				{
					// Create UK2Node_InputAction for action mappings
					UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(EventGraph);
					InputActionNode->CreateNewGuid();
					InputActionNode->InputActionName = FName(*InputName);
					InputActionNode->NodePosX = 0;
					InputActionNode->NodePosY = EventY;
					InputActionNode->AllocateDefaultPins();
					EventGraph->AddNode(InputActionNode, false, false);
					EventNode = InputActionNode;
				}

				// Wire the correct exec output pin (Pressed or Released)
				FName ExecPinName = (InputEventType == IE_Released) ? FName(TEXT("Released")) : FName(TEXT("Pressed"));
				for (UEdGraphPin* Pin : EventNode->Pins)
				{
					if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						if (Pin->PinName == ExecPinName)
						{
							EventExecOut = Pin;
							break;
						}
					}
				}
				if (!EventExecOut)
				{
					EventExecOut = FindExecPin(EventNode, EGPD_Output);
				}

				bHandledAsInputEvent = true;
				UE_LOG(LogGraphBuilder, Log, TEXT("Input %s event: %s (%s)"),
					bIsKeyEvent ? TEXT("Key") : TEXT("Action"),
					*InputName,
					(InputEventType == IE_Released) ? TEXT("Released") : TEXT("Pressed"));
			}
		}
		else if (EventNameStr.StartsWith(TEXT("InpAxisEvt_")))
		{
			// Input Axis event
			// Pattern: InpAxisEvt_<AxisName>_K2Node_InputAxisEvent_<N>
			FString Remainder = EventNameStr.Mid(11); // After "InpAxisEvt_"

			FString AxisName;
			int32 SepIdx = Remainder.Find(TEXT("_K2Node_InputAxisEvent_"));
			if (SepIdx != INDEX_NONE)
			{
				AxisName = Remainder.Left(SepIdx);
			}

			if (!AxisName.IsEmpty())
			{
				UK2Node_InputAxisEvent* InputAxisNode = NewObject<UK2Node_InputAxisEvent>(EventGraph);
				InputAxisNode->CreateNewGuid();
				InputAxisNode->InputAxisName = FName(*AxisName);
				InputAxisNode->NodePosX = 0;
				InputAxisNode->NodePosY = EventY;
				InputAxisNode->AllocateDefaultPins();
				EventGraph->AddNode(InputAxisNode, false, false);
				EventNode = InputAxisNode;
				EventExecOut = FindExecPin(InputAxisNode, EGPD_Output);

				bHandledAsInputEvent = true;
				UE_LOG(LogGraphBuilder, Log, TEXT("Input Axis event: %s"), *AxisName);
			}
		}
		else if (EventNameStr.StartsWith(TEXT("InpAxisKeyEvt_")))
		{
			// Input Axis Key event (specific key generating axis value, e.g. MouseWheelAxis)
			// Pattern: InpAxisKeyEvt_<KeyName>_K2Node_InputAxisKeyEvent_<N>
			FString Remainder = EventNameStr.Mid(14); // After "InpAxisKeyEvt_"

			FString KeyName;
			int32 SepIdx = Remainder.Find(TEXT("_K2Node_InputAxisKeyEvent_"));
			if (SepIdx != INDEX_NONE)
			{
				KeyName = Remainder.Left(SepIdx);
			}

			if (!KeyName.IsEmpty())
			{
				UK2Node_InputAxisKeyEvent* InputAxisKeyNode = NewObject<UK2Node_InputAxisKeyEvent>(EventGraph);
				InputAxisKeyNode->CreateNewGuid();
				InputAxisKeyNode->AxisKey = FKey(*KeyName);
				InputAxisKeyNode->NodePosX = 0;
				InputAxisKeyNode->NodePosY = EventY;
				InputAxisKeyNode->AllocateDefaultPins();
				EventGraph->AddNode(InputAxisKeyNode, false, false);
				EventNode = InputAxisKeyNode;
				EventExecOut = FindExecPin(InputAxisKeyNode, EGPD_Output);

				bHandledAsInputEvent = true;
				UE_LOG(LogGraphBuilder, Log, TEXT("Input Axis Key event: %s"), *KeyName);
			}
		}
		else if (EventNameStr.Contains(TEXT("__UpdateFunc")) || EventNameStr.Contains(TEXT("__FinishedFunc")))
		{
			// Timeline event stubs: <TimelineName>__UpdateFunc / <TimelineName>__FinishedFunc
			// These should be merged into a single UK2Node_Timeline node.
			bool bIsUpdate = EventNameStr.Contains(TEXT("__UpdateFunc"));
			FString TimelineName;
			if (bIsUpdate)
			{
				int32 SepIdx = EventNameStr.Find(TEXT("__UpdateFunc"));
				if (SepIdx != INDEX_NONE) TimelineName = EventNameStr.Left(SepIdx);
			}
			else
			{
				int32 SepIdx = EventNameStr.Find(TEXT("__FinishedFunc"));
				if (SepIdx != INDEX_NONE) TimelineName = EventNameStr.Left(SepIdx);
			}

			if (!TimelineName.IsEmpty())
			{
				// Create or reuse the timeline node
				UK2Node_Timeline** ExistingPtr = TimelineNodes.Find(TimelineName);
				UK2Node_Timeline* TimelineNode = ExistingPtr ? *ExistingPtr : nullptr;

				if (!TimelineNode)
				{
					TimelineNode = NewObject<UK2Node_Timeline>(EventGraph);
					TimelineNode->CreateNewGuid();
					TimelineNode->TimelineName = FName(*TimelineName);
					TimelineNode->NodePosX = 0;
					TimelineNode->NodePosY = EventY;
					TimelineNode->AllocateDefaultPins();
					EventGraph->AddNode(TimelineNode, false, false);
					TimelineNodes.Add(TimelineName, TimelineNode);

					UE_LOG(LogGraphBuilder, Log, TEXT("Created Timeline node: %s"), *TimelineName);
				}

				EventNode = TimelineNode;

				// Wire the correct output pin: Update or Finished
				FName PinName = bIsUpdate ? FName(TEXT("Update")) : FName(TEXT("Finished"));
				for (UEdGraphPin* Pin : TimelineNode->Pins)
				{
					if (Pin->Direction == EGPD_Output &&
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
						Pin->PinName == PinName)
					{
						EventExecOut = Pin;
						break;
					}
				}
				if (!EventExecOut)
				{
					EventExecOut = FindExecPin(TimelineNode, EGPD_Output);
				}

				bHandledAsInputEvent = true;
				UE_LOG(LogGraphBuilder, Log, TEXT("Timeline %s event: %s"),
					bIsUpdate ? TEXT("Update") : TEXT("Finished"), *TimelineName);
			}
		}

		// Declare in outer scope so parameter registration can use them
		UFunction* ParentFunc = nullptr;
		bool bIsOverrideEvent = false;

		if (!bHandledAsInputEvent)
		{

		// ── Component Bound Event detection ──
		// Events with names like "BndEvt__<Component>_K2Node_ComponentBoundEvent_<Idx>_<SigName>__DelegateSignature"
		// are component delegate bindings (OnComponentBeginOverlap, OnComponentHit, etc.).
		// These need UK2Node_ComponentBoundEvent, not K2Node_CustomEvent, so the compiler
		// registers the delegate binding with the component during construction.
		bool bHandledAsComponentBoundEvent = false;
		if (EventNameStr.StartsWith(TEXT("BndEvt__")))
		{
			// Parse: BndEvt__<BlueprintName>_<ComponentName>_K2Node_ComponentBoundEvent_<Index>_<SigFuncName>
			// UE4 format: FString::Printf("BndEvt__%s_%s_%s_%s", *BlueprintName, *ComponentPropName, *NodeName, *DelegateSigName)
			FString AfterPrefix = EventNameStr.Mid(8); // Skip "BndEvt__"
			int32 BoundEventMarker = AfterPrefix.Find(TEXT("_K2Node_ComponentBoundEvent_"));

			if (BoundEventMarker != INDEX_NONE)
			{
				FString ComponentName = AfterPrefix.Left(BoundEventMarker);

				// ComponentName currently = "{BlueprintName}_{ActualComponentName}".
				// Strip the Blueprint asset name prefix so we get the actual property name.
				// The BndEvt may have been created in a PARENT blueprint, so the prefix
				// could be any ancestor's name, not just the current class's name.
				FString OriginalComponentName = ComponentName; // preserve for fallback
				bool bStrippedPrefix = false;
				for (UClass* TestClass = OriginalClass; TestClass && !bStrippedPrefix; TestClass = TestClass->GetSuperClass())
				{
					FString TestClassName = TestClass->GetName();
					TestClassName.RemoveFromEnd(TEXT("_C"));
					if (ComponentName.StartsWith(TestClassName + TEXT("_")))
					{
						ComponentName = ComponentName.Mid(TestClassName.Len() + 1);
						bStrippedPrefix = true;
					}
				}
				FString AfterMarker = AfterPrefix.Mid(BoundEventMarker + 28); // Skip "_K2Node_ComponentBoundEvent_"

				// Skip the index digit(s) and underscore: "0_ComponentBeginOverlapSignature__DelegateSignature"
				int32 FirstUnderscore;
				if (AfterMarker.FindChar(TEXT('_'), FirstUnderscore))
				{
					FString DelegateSigFuncName = AfterMarker.Mid(FirstUnderscore + 1);
					// DelegateSigFuncName = "ComponentBeginOverlapSignature__DelegateSignature"

					// Find the component property on the original class
					FObjectProperty* ComponentProp = nullptr;
					for (TFieldIterator<FObjectProperty> It(OriginalClass); It; ++It)
					{
						if (It->GetFName() == FName(*ComponentName))
						{
							ComponentProp = *It;
							break;
						}
					}

					// Fallback 1: if prefix stripping didn't find the right property,
					// try progressively removing prefix segments separated by underscores.
					// e.g., "CamAccessPoint_PlayerTrigger" → try "PlayerTrigger"
					if (!ComponentProp && !bStrippedPrefix)
					{
						FString TrimmedName = OriginalComponentName;
						int32 UnderPos;
						while (!ComponentProp && TrimmedName.FindChar(TEXT('_'), UnderPos))
						{
							TrimmedName = TrimmedName.Mid(UnderPos + 1);
							for (TFieldIterator<FObjectProperty> It(OriginalClass); It; ++It)
							{
								if (It->GetFName() == FName(*TrimmedName))
								{
									ComponentProp = *It;
									ComponentName = TrimmedName;
									break;
								}
							}
						}
					}

					// Fallback 2: The BndEvt name may use the component template's UObject
					// name (e.g., "Sphere" from USphereComponent) rather than the SCS
					// variable/property name (e.g., "EnemyTrigger"). Search CDO subobjects
					// for a component whose UObject name matches.
					if (!ComponentProp)
					{
						UObject* CDO = OriginalClass->GetDefaultObject();
						if (CDO)
						{
							for (TFieldIterator<FObjectProperty> It(OriginalClass); It; ++It)
							{
								UObject* DefaultComp = It->GetObjectPropertyValue(
									It->ContainerPtrToValuePtr<void>(CDO));
								if (DefaultComp && DefaultComp->GetName() == ComponentName)
								{
									ComponentProp = *It;
									UE_LOG(LogGraphBuilder, Log, TEXT("BndEvt component '%s' matched via CDO subobject name -> property '%s'"),
										*ComponentName, *It->GetName());
									ComponentName = It->GetName();
									break;
								}
							}
						}
					}

					// Fallback 3: The BndEvt name may use the default auto-generated name
					// derived from the component class (e.g., USphereComponent → "Sphere",
					// or "Sphere1", "Sphere2" for multiple instances of the same type).
					// The developer may have renamed the component after creating the bound
					// event, but the bytecode keeps the original class-derived name.
					if (!ComponentProp)
					{
						// Collect all properties matching by class-derived name.
						// UE4 auto-names: "Sphere", "Sphere1", "Sphere2", etc.
						// We need to match the Nth instance for numbered variants.
						TArray<FObjectProperty*> ClassMatches;
						FString BaseClassName; // The class-derived base (e.g., "Sphere")

						for (TFieldIterator<FObjectProperty> It(OriginalClass); It; ++It)
						{
							if (!It->PropertyClass) continue;
							FString ClassName = It->PropertyClass->GetName();
							ClassName.RemoveFromEnd(TEXT("Component"));

							// Check: ComponentName == ClassName (e.g., "Sphere")
							// OR ComponentName == ClassName + digits (e.g., "Sphere1")
							if (ComponentName == ClassName)
							{
								BaseClassName = ClassName;
								ClassMatches.Add(*It);
							}
							else if (ComponentName.StartsWith(ClassName) && ComponentName.Len() > ClassName.Len())
							{
								// Check if the suffix is all digits
								FString Suffix = ComponentName.Mid(ClassName.Len());
								bool bAllDigits = true;
								for (TCHAR Ch : Suffix)
								{
									if (!FChar::IsDigit(Ch)) { bAllDigits = false; break; }
								}
								if (bAllDigits)
								{
									BaseClassName = ClassName;
									ClassMatches.Add(*It);
								}
							}
						}

						if (ClassMatches.Num() > 0)
						{
							// Determine which instance: "Sphere" = index 0, "Sphere1" = index 1, etc.
							int32 InstanceIndex = 0;
							if (ComponentName.Len() > BaseClassName.Len())
							{
								FString Suffix = ComponentName.Mid(BaseClassName.Len());
								InstanceIndex = FCString::Atoi(*Suffix);
							}

							// Collect ALL properties of this component class type, in field order
							TArray<FObjectProperty*> AllOfType;
							for (TFieldIterator<FObjectProperty> It(OriginalClass); It; ++It)
							{
								if (It->PropertyClass && It->PropertyClass->GetName().StartsWith(BaseClassName))
								{
									AllOfType.Add(*It);
								}
							}

							if (InstanceIndex < AllOfType.Num())
							{
								ComponentProp = AllOfType[InstanceIndex];
							}
							else if (AllOfType.Num() > 0)
							{
								ComponentProp = AllOfType[0]; // Best effort
							}

							if (ComponentProp)
							{
								UE_LOG(LogGraphBuilder, Log, TEXT("BndEvt component '%s' matched via class name '%s' (instance %d) -> property '%s'"),
									*ComponentName, *BaseClassName, InstanceIndex, *ComponentProp->GetName());
								ComponentName = ComponentProp->GetName();
							}
						}
					}

					if (ComponentProp && ComponentProp->PropertyClass)
					{
						UClass* ComponentClass = ComponentProp->PropertyClass;

						// Method 1: Collect ALL delegate properties whose SignatureFunction matches.
						// Multiple delegate properties can share the same signature type
						// (e.g., OnOpenDoor and OnCloseDoor both use FOnOpenCloseDoor,
						// which has SignatureFunction named "OnOpenCloseDoor__DelegateSignature").
						// We collect all matches and use a counter to assign each BndEvt stub
						// to the next unassigned delegate property in declaration order.
						TArray<FMulticastDelegateProperty*> MatchingDelegates;
						for (TFieldIterator<FProperty> It(ComponentClass); It; ++It)
						{
							FMulticastDelegateProperty* MCDP = CastField<FMulticastDelegateProperty>(*It);
							if (MCDP && MCDP->SignatureFunction)
							{
								if (MCDP->SignatureFunction->GetName() == DelegateSigFuncName)
								{
									MatchingDelegates.Add(MCDP);
								}
							}
						}

						// Pick the correct delegate property based on how many BndEvt stubs
						// with this signature we've already processed for this component.
						FMulticastDelegateProperty* DelegateProp = nullptr;
						if (MatchingDelegates.Num() > 0)
						{
							FString SignatureKey = ComponentName + TEXT("::") + DelegateSigFuncName;
							int32& Counter = BoundEventSignatureCounters.FindOrAdd(SignatureKey);
							int32 Index = Counter % MatchingDelegates.Num();
							DelegateProp = MatchingDelegates[Index];
							Counter++;

							if (MatchingDelegates.Num() > 1)
							{
								UE_LOG(LogGraphBuilder, Log, TEXT("Multiple delegates (%d) share signature '%s' on component '%s' — assigning BndEvt to delegate[%d]='%s'"),
									MatchingDelegates.Num(), *DelegateSigFuncName, *ComponentName, Index, *DelegateProp->GetName());
							}
						}

						// Method 2: Name heuristic fallback — derive property name from signature name
						// "ComponentBeginOverlapSignature__DelegateSignature" → strip suffix → "ComponentBeginOverlapSignature"
						// → strip "Signature" → "ComponentBeginOverlap" → prepend "On" → "OnComponentBeginOverlap"
						if (!DelegateProp)
						{
							FString DelegateSigName = DelegateSigFuncName;
							DelegateSigName.RemoveFromEnd(TEXT("__DelegateSignature"));
							DelegateSigName.RemoveFromEnd(TEXT("Signature"));
							FString DelegatePropName = TEXT("On") + DelegateSigName;

							for (TFieldIterator<FProperty> It(ComponentClass); It; ++It)
							{
								if (It->GetFName() == FName(*DelegatePropName))
								{
									DelegateProp = CastField<FMulticastDelegateProperty>(*It);
									break;
								}
								// Also try without "On" prefix
								if (It->GetFName() == FName(*DelegateSigName))
								{
									DelegateProp = CastField<FMulticastDelegateProperty>(*It);
									break;
								}
							}
						}

						if (DelegateProp)
						{
							UK2Node_ComponentBoundEvent* BoundEvent = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
							BoundEvent->CreateNewGuid();
							BoundEvent->InitializeComponentBoundEventParams(ComponentProp, DelegateProp);
							BoundEvent->NodePosX = 0;
							BoundEvent->NodePosY = EventY;
							BoundEvent->AllocateDefaultPins();
							EventGraph->AddNode(BoundEvent, false, false);
							EventNode = BoundEvent;
							EventExecOut = FindExecPin(BoundEvent, EGPD_Output);
							bHandledAsComponentBoundEvent = true;

							UE_LOG(LogGraphBuilder, Log, TEXT("Created ComponentBoundEvent: component=%s delegate=%s on %s"),
								*ComponentName, *DelegateProp->GetName(), *ComponentClass->GetName());
						}
						else
						{
							Warn(FString::Printf(TEXT("Could not find delegate property for component bound event '%s' on %s — falling back to CustomEvent"),
								*EventNameStr, *ComponentClass->GetName()));
						}
					}
					else
					{
						Warn(FString::Printf(TEXT("Could not find component property '%s' on %s for bound event"),
							*ComponentName, *OriginalClass->GetName()));
					}
				}
			}
		}

		if (!bHandledAsComponentBoundEvent)
		{
		// ── Existing override/custom event detection ──
		// Determine if this is a parent/interface-defined event (override) or a custom event
		// Check parent class hierarchy for a function with this name

		if (bBuildingChildClass && OriginalClass)
		{
			// Child class mode: the child's parent IS OriginalClass.
			// ALL events defined on OriginalClass are overrides in the child.
			// First check the grandparent (OriginalClass->GetSuperClass())
			if (OriginalClass->GetSuperClass())
			{
				ParentFunc = OriginalClass->GetSuperClass()->FindFunctionByName(EventName);
				if (ParentFunc && ParentFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					bIsOverrideEvent = true;
				}
			}
			// Then check OriginalClass itself (the child's direct parent).
			// Custom events on the cooked class become overrides in the child.
			if (!bIsOverrideEvent)
			{
				ParentFunc = OriginalClass->FindFunctionByName(EventName);
				if (ParentFunc)
				{
					bIsOverrideEvent = true;
				}
			}
		}
		else if (OriginalClass && OriginalClass->GetSuperClass())
		{
			ParentFunc = OriginalClass->GetSuperClass()->FindFunctionByName(EventName);
			if (ParentFunc && ParentFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				bIsOverrideEvent = true;
			}
		}

		// Also check interfaces
		if (!bIsOverrideEvent && OriginalClass)
		{
			for (const FImplementedInterface& Iface : OriginalClass->Interfaces)
			{
				if (Iface.Class)
				{
					UFunction* IfaceFunc = Iface.Class->FindFunctionByName(EventName);
					if (IfaceFunc)
					{
						ParentFunc = IfaceFunc;
						bIsOverrideEvent = true;
						break;
					}
				}
			}
		}

		// Also treat Receive* events as overrides even if parent func wasn't found
		// (they're defined in AActor/APawn which might not have FUNC_BlueprintEvent flag)
		if (!bIsOverrideEvent && EventName.ToString().StartsWith(TEXT("Receive")))
		{
			bIsOverrideEvent = true;
		}

		if (bIsOverrideEvent && (StubFunction || (bBuildingChildClass && ParentFunc)))
		{
			// Create a standard event node (override of parent/interface function)
			UK2Node_Event* Event = NewObject<UK2Node_Event>(EventGraph);
			Event->CreateNewGuid();

			if (ParentFunc)
			{
				// Walk up the super function chain to find the original C++ declaring class.
				// If ParentFunc belongs to a BlueprintGeneratedClass (e.g. FirstPersonCharacter_C),
				// EventReference must point to the native C++ class that originally defined the event
				// (e.g. AActor for ReceiveTick). Otherwise the compiler can't create the persistent
				// frame properties and we get ICE SetVariableOnPersistentFrame errors.
				UFunction* RootFunc = ParentFunc;
				while (UFunction* SuperFunc = RootFunc->GetSuperFunction())
				{
					RootFunc = SuperFunc;
				}
				UClass* DeclaringClass = RootFunc->GetOwnerClass();

				// If the declaring class is a BPGC, use its GeneratedClass (the authoritative class)
				if (UBlueprint* OwnerBP = Cast<UBlueprint>(DeclaringClass->ClassGeneratedBy))
				{
					if (OwnerBP->GeneratedClass)
					{
						DeclaringClass = OwnerBP->GeneratedClass;
					}
				}

				Event->EventReference.SetExternalMember(EventName, DeclaringClass);
			}
			else if (OriginalClass->GetSuperClass())
			{
				// Walk up to a native class — using a BPGC here causes the same ICE issues
				UClass* FallbackClass = OriginalClass->GetSuperClass();
				while (FallbackClass && FallbackClass->ClassGeneratedBy)
				{
					FallbackClass = FallbackClass->GetSuperClass();
				}
				if (!FallbackClass)
				{
					FallbackClass = OriginalClass->GetSuperClass();
				}
				Event->EventReference.SetExternalMember(EventName, FallbackClass);
			}

			Event->bOverrideFunction = true;
			Event->NodePosX = 0;
			Event->NodePosY = EventY;
			Event->AllocateDefaultPins();
			EventGraph->AddNode(Event, false, false);
			EventNode = Event;
			EventExecOut = FindExecPin(Event, EGPD_Output);
		}
		else
		{
			// Create a custom event node
			UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
			CustomEvent->CreateNewGuid();
			CustomEvent->CustomFunctionName = EventName;
			CustomEvent->NodePosX = 0;
			CustomEvent->NodePosY = EventY;
			CustomEvent->AllocateDefaultPins();

			// Add parameter pins if the stub function has parameters
			if (StubFunction)
			{
				for (TFieldIterator<FProperty> ParamIt(StubFunction); ParamIt; ++ParamIt)
				{
					FProperty* Param = *ParamIt;
					if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
						continue;

					FEdGraphPinType PinType;
					GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Param, PinType);
					CustomEvent->CreateUserDefinedPin(Param->GetFName(), PinType, EGPD_Output);
				}
			}

			EventGraph->AddNode(CustomEvent, false, false);
			EventNode = CustomEvent;
			EventExecOut = FindExecPin(CustomEvent, EGPD_Output);
		}
		} // end !bHandledAsComponentBoundEvent
		} // end !bHandledAsInputEvent

		UE_LOG(LogGraphBuilder, Log, TEXT("Event: %s (statements %d-%d, offset 0x%04X)"),
			*EventName.ToString(), StartStatementIdx, EndStatementIdx, EntryOffset);

		// Clear local variable tracking for this event region and register event parameter pins
		ClearLocalVarMap();
		UFunction* ParameterFunc = StubFunction ? StubFunction : ParentFunc;
		if (ParameterFunc && EventNode)
		{
			for (TFieldIterator<FProperty> ParamIt(ParameterFunc); ParamIt; ++ParamIt)
			{
				FProperty* Param = *ParamIt;
				if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
					continue;

				FName ParamName = Param->GetFName();
				for (UEdGraphPin* Pin : EventNode->Pins)
				{
					if (Pin->Direction == EGPD_Output && Pin->PinName == ParamName)
					{
						LocalVarToPinMap.Add(ParamName, Pin);
						break;
					}
				}
			}
		}

		// Fallback: register event parameters from the PARENT function signature.
		// On cooked BPGCs, event stub functions may have their CPF_Parm flags stripped
		// from parameters, causing the above registration to find nothing. For override
		// events, the event node's pins come from the parent function, so we can match
		// parent param names to event output pins directly.
		if (bIsOverrideEvent && ParentFunc && EventNode)
		{
			for (TFieldIterator<FProperty> ParamIt(ParentFunc); ParamIt; ++ParamIt)
			{
				FProperty* Param = *ParamIt;
				if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
					continue;

				FName ParamName = Param->GetFName();
				// Skip if already registered from the stub function
				if (LocalVarToPinMap.Contains(ParamName))
					continue;

				for (UEdGraphPin* Pin : EventNode->Pins)
				{
					if (Pin->Direction == EGPD_Output && Pin->PinName == ParamName)
					{
						LocalVarToPinMap.Add(ParamName, Pin);
						UE_LOG(LogGraphBuilder, Log, TEXT("Registered event param '%s' from parent function %s"),
							*ParamName.ToString(), *ParentFunc->GetName());
						break;
					}
				}
			}
		}

		// Compute the event entry's statement index for proper processing order.
		// When backward jumps pull StartStatementIdx below the entry, EmitStatements
		// needs to know where the event actually starts to connect IncomingExecPin correctly.
		int32 EventEntryStmtIdx = -1;
		{
			int32* EntryIdxPtr = OffsetToStmtIdx.Find(EntryOffset);
			if (EntryIdxPtr)
			{
				EventEntryStmtIdx = *EntryIdxPtr;
			}
		}

		if (EventEntryStmtIdx >= 0 && EventEntryStmtIdx > StartStatementIdx)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("Event '%s': backward-jump detected — reordering processing. Entry stmt %d, range [%d, %d)"),
				*EventName.ToString(), EventEntryStmtIdx, StartStatementIdx, EndStatementIdx);
		}

		// Emit the statements for this event (filtered to only reachable statements)
		EmitStatements(EventGraph, UbergraphFunc.Statements,
			StartStatementIdx, EndStatementIdx,
			NodeSpacingX, EventY, EventExecOut,
			pReachable, EventEntryStmtIdx);

		EventY += NodeSpacingY * 4; // Space between event regions
	}

	// If there are no event entry points but we still have statements, emit them all
	if (SortedEntries.Num() == 0 && UbergraphFunc.Statements.Num() > 0)
	{
		Warn(TEXT("Ubergraph has statements but no identified event entry points. Emitting as linear graph."));
		EmitStatements(EventGraph, UbergraphFunc.Statements,
			0, UbergraphFunc.Statements.Num(),
			0, 0, nullptr);
	}
}

// Function Graph Building

void FGraphBuilder::BuildFunctionGraph(UBlueprint* BP, const FDecompiledFunction& Func, UClass* OriginalClass)
{
	// Skip ExecuteUbergraph - it's handled via BuildEventGraph
	if (Func.FunctionName.StartsWith(TEXT("ExecuteUbergraph_")))
		return;

	// Skip timeline stubs - they're handled as UK2Node_Timeline in BuildEventGraph
	if (Func.FunctionName.Contains(TEXT("__UpdateFunc")) || Func.FunctionName.Contains(TEXT("__FinishedFunc")))
		return;

	FName GraphName(*Func.FunctionName);

	// Check if this function already exists as an event in the ubergraph.
	// Events are legitimately already built — skip them.
	for (UEdGraph* Existing : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Existing->Nodes)
		{
			UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
			if (EventNode && EventNode->GetFunctionName() == GraphName)
			{
				UE_LOG(LogGraphBuilder, Log, TEXT("Function '%s' already exists as event in ubergraph, skipping"),
					*Func.FunctionName);
				return;
			}
			UK2Node_CustomEvent* CustomNode = Cast<UK2Node_CustomEvent>(Node);
			if (CustomNode && CustomNode->CustomFunctionName == GraphName)
			{
				UE_LOG(LogGraphBuilder, Log, TEXT("Function '%s' already exists as custom event in ubergraph, skipping"),
					*Func.FunctionName);
				return;
			}
		}
	}

	// If a function graph with this name already exists (e.g. the default
	// UserConstructionScript created by CreateBlueprint), remove it properly
	// so we can replace it with the decompiled version.
	// Must use FBlueprintEditorUtils::RemoveGraph to clean up sub-objects,
	// otherwise CreateNewGraph hits an ensure for the duplicate name.
	for (int32 i = BP->FunctionGraphs.Num() - 1; i >= 0; --i)
	{
		if (BP->FunctionGraphs[i] && BP->FunctionGraphs[i]->GetFName() == GraphName)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("Removing existing default function graph '%s' to replace with decompiled version"),
				*Func.FunctionName);
			FBlueprintEditorUtils::RemoveGraph(BP, BP->FunctionGraphs[i]);
			break;
		}
	}

	// NOTE: Interface functions are NOT skipped. They are built here with the
	// decompiled logic, then moved to InterfaceDesc.Graphs by BuildBlueprint/
	// BuildChildBlueprint/LiveBuildBlueprint. ConformImplementedInterfaces will
	// find the populated graph and use it instead of creating an empty stub.

	UE_LOG(LogGraphBuilder, Log, TEXT("Building function graph: %s (%d statements)"),
		*Func.FunctionName, Func.Statements.Num());

	// Create a new function graph
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(BP,
		GraphName,
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());

	// Add to the blueprint's function graph list
	BP->FunctionGraphs.Add(FuncGraph);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Manually create the FunctionEntry node (since we're not using AddFunctionGraph)
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : FuncGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		// Create one manually
		EntryNode = NewObject<UK2Node_FunctionEntry>(FuncGraph);
		EntryNode->CreateNewGuid();
		EntryNode->NodePosX = 0;
		EntryNode->NodePosY = 0;
		EntryNode->CustomGeneratedFunctionName = GraphName;
		EntryNode->AllocateDefaultPins();
		FuncGraph->AddNode(EntryNode, false, false);
	}

	// Always ensure the function name is set (even if the node already existed)
	if (EntryNode->CustomGeneratedFunctionName == NAME_None)
	{
		EntryNode->CustomGeneratedFunctionName = GraphName;
	}

	// Set the function reference to point to the PARENT function if this is an override.
	// Do NOT use OriginalClass here — that's the cooked Gregory_C, not our parent.
	// Only set FunctionReference for functions inherited from the parent class.
	bool bIsOverride = false;
	if (Func.OriginalFunction && BP->ParentClass)
	{
		UFunction* ParentFunc = BP->ParentClass->FindFunctionByName(GraphName);
		if (ParentFunc)
		{
			// This is an override of a parent function — reference the parent's version
			bIsOverride = true;
			EntryNode->CustomGeneratedFunctionName = NAME_None;
			EntryNode->FunctionReference.SetExternalMember(GraphName, ParentFunc->GetOwnerClass());
			EntryNode->Pins.Empty();
			EntryNode->AllocateDefaultPins();
		}
		// else: new function on this class, CustomGeneratedFunctionName is correct
	}

	// Check if this function implements an interface function.
	// Interface implementations need FunctionReference set to the interface's
	// declaring class so AllocateDefaultPins gets the correct signature and
	// the compiler recognizes this as an interface implementation.
	if (!bIsOverride && OriginalClass)
	{
		for (const FImplementedInterface& Iface : OriginalClass->Interfaces)
		{
			if (Iface.Class)
			{
				UFunction* IfaceFunc = Iface.Class->FindFunctionByName(GraphName);
				if (IfaceFunc)
				{
					bIsOverride = true;
					EntryNode->CustomGeneratedFunctionName = NAME_None;
					EntryNode->FunctionReference.SetExternalMember(GraphName, Iface.Class);
					EntryNode->Pins.Empty();
					EntryNode->AllocateDefaultPins();
					UE_LOG(LogGraphBuilder, Log, TEXT("Function '%s' implements interface '%s'"),
						*Func.FunctionName, *Iface.Class->GetName());
					break;
				}
			}
		}
	}

	// Add parameter pins based on the original function signature (only for NEW functions, 
	// not overrides — overrides already got pins from AllocateDefaultPins via FunctionReference)
	if (Func.OriginalFunction && !bIsOverride)
	{
		UFunction* OrigFunc = Func.OriginalFunction;

		// Set function flags
		if (OrigFunc->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			// Mark as pure - TODO: FBlueprintEditorUtils method
		}

		// Add output parameters to a result node if needed
		bool bHasReturnValue = false;
		for (TFieldIterator<FProperty> It(OrigFunc); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				bHasReturnValue = true;
				break;
			}
		}

		// Add user-defined pins for parameters
		for (TFieldIterator<FProperty> It(OrigFunc); It; ++It)
		{
			FProperty* Param = *It;
			if (!Param->HasAnyPropertyFlags(CPF_Parm)) continue;
			if (Param->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

			// Input params go on the entry node, out params go on result node
			// For now, add all as entry pins
			FEdGraphPinType PinType;
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Param, PinType);

			if (Param->HasAnyPropertyFlags(CPF_OutParm))
			{
				// Output param - we'd add to result node
				// For Phase 1, skip
			}
			else
			{
				EntryNode->CreateUserDefinedPin(Param->GetFName(), PinType, EGPD_Output);
			}
		}

		// Also add parameters that come from event stubs via the persistent frame.
		// These are local variables in this function whose names match event stub
		// parameters — they're passed from the ubergraph but don't have CPF_Parm
		// on this function because the compiler uses persistent frame storage.
		if (EventStubParamNames.Num() > 0)
		{
			TSet<FName> AlreadyAdded;
			for (TFieldIterator<FProperty> It(OrigFunc); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					AlreadyAdded.Add(It->GetFName());
				}
			}

			for (TFieldIterator<FProperty> It(OrigFunc); It; ++It)
			{
				FProperty* Local = *It;
				FName LocalName = Local->GetFName();

				// Skip if already added as a regular parameter
				if (AlreadyAdded.Contains(LocalName)) continue;
				// Skip compiler temporaries
				FString NameStr = LocalName.ToString();
				if (NameStr.StartsWith(TEXT("Temp_")) || NameStr.Contains(TEXT("K2Node_")) ||
					NameStr.Contains(TEXT("CallFunc_"))) continue;

				// Check if this local matches an event stub parameter
				if (EventStubParamNames.Contains(LocalName))
				{
					FEdGraphPinType PinType;
					GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Local, PinType);
					EntryNode->CreateUserDefinedPin(LocalName, PinType, EGPD_Output);
					UE_LOG(LogGraphBuilder, Log, TEXT("  Added persistent frame parameter '%s' from event stub"),
						*LocalName.ToString());
				}
			}
		}
	}

	EntryNode->NodePosX = 0;
	EntryNode->NodePosY = 0;
	EntryNode->ReconstructNode();

	// Get the exec output pin of the entry node
	UEdGraphPin* EntryExecOut = FindExecPin(EntryNode, EGPD_Output);

	// Clear local variable tracking and register function parameter pins
	// so that bytecode reads of parameter locals wire to entry node outputs.
	// Also clear PersistentFrameVarMap — it accumulates pins across events
	// within the event graph, but function graphs are standalone and must NOT
	// see event graph pins (causes cross-graph pin connection ensure failures).
	ClearLocalVarMap();
	PersistentFrameVarMap.Reset();
	if (Func.OriginalFunction)
	{
		for (TFieldIterator<FProperty> ParamIt(Func.OriginalFunction); ParamIt; ++ParamIt)
		{
			FProperty* Param = *ParamIt;
			if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
				continue;
			if (Param->HasAnyPropertyFlags(CPF_OutParm))
				continue;

			// Find the matching output pin on the entry node
			FName ParamName = Param->GetFName();
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->PinName == ParamName)
				{
					LocalVarToPinMap.Add(ParamName, Pin);
					break;
				}
			}
		}

		// Also register event stub persistent frame parameters in the pin map
		// so EX_LocalOutVariable references wire to the entry node output
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
				!LocalVarToPinMap.Contains(Pin->PinName))
			{
				if (EventStubParamNames.Contains(Pin->PinName))
				{
					LocalVarToPinMap.Add(Pin->PinName, Pin);
				}
			}
		}
	}

	// Emit statements
	EmitStatements(FuncGraph, Func.Statements,
		0, Func.Statements.Num(),
		NodeSpacingX, 0, EntryExecOut);

	// Check if the function has return value or out parameters → create FunctionResult node
	if (Func.OriginalFunction)
	{
		bool bHasReturnValue = false;
		bool bHasOutParams = false;
		FProperty* ReturnProp = nullptr;

		for (TFieldIterator<FProperty> It(Func.OriginalFunction); It; ++It)
		{
			FProperty* Param = *It;
			if (!Param->HasAnyPropertyFlags(CPF_Parm)) continue;
			if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				bHasReturnValue = true;
				ReturnProp = Param;
			}
			else if (Param->HasAnyPropertyFlags(CPF_OutParm))
			{
				bHasOutParams = true;
			}
		}

		if (bHasReturnValue || bHasOutParams)
		{
			UK2Node_FunctionResult* ResultNode = NewObject<UK2Node_FunctionResult>(FuncGraph);
			ResultNode->CreateNewGuid();

			if (bIsOverride && BP->ParentClass)
			{
				// For overrides, reference the parent function so the result node
				// gets the correct signature automatically (avoids "Cannot order parameters" error)
				UFunction* ParFunc = BP->ParentClass->FindFunctionByName(GraphName);
				if (ParFunc)
				{
					ResultNode->FunctionReference.SetExternalMember(GraphName, ParFunc->GetOwnerClass());
				}
				else
				{
					ResultNode->FunctionReference.SetSelfMember(GraphName);
				}
			}
			else
			{
				// For full uncook, use external member pointing to the original
				// function's owner class so AllocateDefaultPins/ReconstructNode
				// can resolve the function. SetSelfMember fails here because the
				// new BP's GeneratedClass doesn't have this function yet.
				if (Func.OriginalFunction && Func.OriginalFunction->GetOwnerClass())
				{
					ResultNode->FunctionReference.SetExternalMember(GraphName, Func.OriginalFunction->GetOwnerClass());
				}
				else
				{
					ResultNode->FunctionReference.SetSelfMember(GraphName);
				}
			}

			ResultNode->NodePosX = NodeSpacingX + Func.Statements.Num() * NodeSpacingX;
			ResultNode->NodePosY = 0;
			ResultNode->AllocateDefaultPins();

			// Only add manual pins for NON-override functions (overrides get pins from parent signature)
			if (!bIsOverride)
			{
				for (TFieldIterator<FProperty> It(Func.OriginalFunction); It; ++It)
				{
					FProperty* Param = *It;
					if (!Param->HasAnyPropertyFlags(CPF_Parm)) continue;
					if (Param->HasAnyPropertyFlags(CPF_ReturnParm) || Param->HasAnyPropertyFlags(CPF_OutParm))
					{
						FEdGraphPinType PinType;
						GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Param, PinType);
						FName PinName = Param->HasAnyPropertyFlags(CPF_ReturnParm)
							? UEdGraphSchema_K2::PN_ReturnValue
							: Param->GetFName();
						ResultNode->CreateUserDefinedPin(PinName, PinType, EGPD_Input);
					}
				}
			}
			ResultNode->ReconstructNode();

			FuncGraph->AddNode(ResultNode, false, false);

			// Wire ALL out-params and return value from LocalVarToPinMap
			// This runs AFTER EmitStatements, so all pins are in the map
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin->Direction != EGPD_Input ||
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ||
					Pin->LinkedTo.Num() > 0)
				{
					continue;  // Skip exec pins, already-wired pins
				}

				// Try exact FName match in LocalVarToPinMap
				FName PinName = Pin->PinName;
				UEdGraphPin** FoundPin = LocalVarToPinMap.Find(PinName);

				// Fallback: try fuzzy match (remove spaces, case-insensitive)
				if (!FoundPin)
				{
					FString PinStr = PinName.ToString().Replace(TEXT(" "), TEXT(""));
					for (auto& Pair : LocalVarToPinMap)
					{
						FString VarStr = Pair.Key.ToString().Replace(TEXT(" "), TEXT(""));
						if (VarStr.Equals(PinStr, ESearchCase::IgnoreCase))
						{
							FoundPin = &Pair.Value;
							break;
						}
					}
				}

				if (FoundPin && *FoundPin)
				{
					ForceConnect(*FoundPin, Pin);
				}
				else
				{
					// No source pin found — check OutParamLiteralMap for constant values.
					// Functions like GetInteractibleType set out-params to literals
					// (e.g., EX_ByteConst 1) which can't produce pins via ResolveDataExpr.
					TSharedPtr<FDecompiledExpr>* LitExpr = OutParamLiteralMap.Find(PinName);

					// Fuzzy match for literal map too
					if (!LitExpr)
					{
						FString PinStr = PinName.ToString().Replace(TEXT(" "), TEXT(""));
						for (auto& Pair : OutParamLiteralMap)
						{
							FString VarStr = Pair.Key.ToString().Replace(TEXT(" "), TEXT(""));
							if (VarStr.Equals(PinStr, ESearchCase::IgnoreCase))
							{
								LitExpr = &Pair.Value;
								break;
							}
						}
					}

					if (LitExpr && LitExpr->IsValid())
					{
						TSharedPtr<FDecompiledExpr> Lit = *LitExpr;

						// Enum-aware: resolve byte/int literals to enum names when pin expects an enum
						bool bEnumResolved = false;
						if ((Lit->Token == EX_ByteConst || Lit->Token == EX_IntConst ||
							Lit->Token == EX_IntConstByte || Lit->Token == EX_IntZero || Lit->Token == EX_IntOne) &&
							Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
							Pin->PinType.PinSubCategoryObject.IsValid())
						{
							UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
							if (PinEnum)
							{
								int64 Val = (Lit->Token == EX_ByteConst) ? Lit->ByteValue : Lit->IntValue;
								FString EnumName = PinEnum->GetNameStringByValue(Val);
								if (!EnumName.IsEmpty())
								{
									Pin->DefaultValue = EnumName;
									bEnumResolved = true;
									UE_LOG(LogGraphBuilder, Log, TEXT("FunctionResult pin '%s' ← enum '%s' (val=%lld from %s)"),
										*PinName.ToString(), *EnumName, Val, *PinEnum->GetName());
								}
							}
						}
						if (!bEnumResolved)
						{
							if (Lit->Token == EX_ObjectConst && Lit->ObjectRef)
							{
								Pin->DefaultObject = Lit->ObjectRef;
							}
							else if (Lit->Token == EX_NoObject)
							{
								Pin->DefaultValue = TEXT("");
							}
							else
							{
								Pin->DefaultValue = Lit->GetLiteralAsString();
							}
							UE_LOG(LogGraphBuilder, Log, TEXT("FunctionResult pin '%s' ← literal default '%s'"),
								*PinName.ToString(), *Pin->DefaultValue);
						}

						// Save for re-application after RefreshAllNodes which can clear defaults
						DeferredPinDefaults.Add({ResultNode, Pin->PinName, Pin->DefaultValue, Pin->DefaultObject});
					}
				}
			}

			// Wire exec flow: all return paths → FunctionResult exec input.
			// ReturnExecSources collects every exec output pin that leads to an
			// EX_Return statement (from linear flow and from deferred branch paths).
			// Exec input pins accept multiple connections, so all return paths converge.
			UEdGraphPin* ResultExecIn = FindExecPin(ResultNode, EGPD_Input);
			if (ResultExecIn)
			{
				for (UEdGraphPin* RetExecOut : ReturnExecSources)
				{
					if (RetExecOut && RetExecOut->LinkedTo.Num() == 0)
					{
						TryConnect(RetExecOut, ResultExecIn);
					}
				}
				// Fallback: if no return sources connected, try LastFunctionExecOut
				if (ResultExecIn->LinkedTo.Num() == 0 && LastFunctionExecOut)
				{
					TryConnect(LastFunctionExecOut, ResultExecIn);
				}
				if (ResultExecIn->LinkedTo.Num() > 0)
				{
					UE_LOG(LogGraphBuilder, Log, TEXT("FunctionResult exec wired from %d source(s)"),
						ResultExecIn->LinkedTo.Num());
				}
				else
				{
					UE_LOG(LogGraphBuilder, Warning, TEXT("FunctionResult exec input has NO connections — function may not return"));
				}
			}
		}

		// Copy function flags to the entry node
		if (Func.OriginalFunction->HasAnyFunctionFlags(FUNC_Const))
		{
			EntryNode->AddExtraFlags(FUNC_Const);
		}
		if (Func.OriginalFunction->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			EntryNode->AddExtraFlags(FUNC_BlueprintPure);
		}
	}
}

// Statement Emission

// Recursively collect Temp_ variable reads from an expression tree.
// Used by the read-before-write pre-scan to detect stateful temporaries
// (e.g., FlipFlop toggle booleans, DoOnce gates, Gate counters) that need
// TemporaryVariable + AssignmentStatement nodes instead of pin aliasing.
static void CollectTempVarReads(TSharedPtr<FDecompiledExpr> Expr, TMap<FName, int32>& OutFirstReadIdx, int32 StmtIdx)
{
	if (!Expr) return;

	if ((Expr->Token == EX_LocalVariable || Expr->Token == EX_InstanceVariable) && Expr->PropertyRef)
	{
		FName VarName = Expr->PropertyRef->GetFName();
		FString VarStr = VarName.ToString();
		if (VarStr.StartsWith(TEXT("Temp_")))
		{
			if (!OutFirstReadIdx.Contains(VarName) || StmtIdx < OutFirstReadIdx[VarName])
			{
				OutFirstReadIdx.FindOrAdd(VarName) = StmtIdx;
			}
		}
	}

	for (auto& Child : Expr->Children)
	{
		CollectTempVarReads(Child, OutFirstReadIdx, StmtIdx);
	}

	if (Expr->ContextObject)
	{
		CollectTempVarReads(Expr->ContextObject, OutFirstReadIdx, StmtIdx);
	}
}

void FGraphBuilder::EmitStatements(
	UEdGraph* Graph,
	const TArray<TSharedPtr<FDecompiledExpr>>& Statements,
	int32 StartIndex, int32 EndIndex,
	int32 BaseX, int32 BaseY,
	UEdGraphPin* IncomingExecPin,
	const TSet<int32>* pReachableStmts,
	int32 EventEntryStmtIdx)
{
	int32 CurrentX = BaseX;
	int32 CurrentY = BaseY;
	UEdGraphPin* PrevExecOut = IncomingExecPin;

	EndIndex = FMath::Min(EndIndex, Statements.Num());

	// Pre-scan: identify local variables assigned more than once in this scope.
	// Loop counters (Temp_int_Loop_Counter_Variable) and accumulators (StunCount, etc.)
	// get initialized then updated each iteration. The LocalVarToPinMap pass-through
	// optimization assumes single-assignment semantics, so multi-assigned variables
	// must use TemporaryVariable + AssignmentStatement instead — otherwise reads
	// always resolve to a stale value and the variable never accumulates.
	{
		TMap<FName, int32> TempAssignCounts;
		for (int32 i = StartIndex; i < EndIndex; i++)
		{
			TSharedPtr<FDecompiledExpr> Stmt = Statements[i];
			if (!Stmt) continue;
			if (pReachableStmts && !pReachableStmts->Contains(i)) continue;

			// Check Let/LetObj/LetBool etc. where destination is a local variable.
			// Track Temp_ compiler vars AND user-defined locals (e.g. StunCount)
			// but NOT CallFunc_/K2Node_ compiler temps — those use single-assignment
			// pass-through and must keep their exec wiring via CreateNodeForExpr.
			if (Stmt->Token == EX_Let || Stmt->Token == EX_LetObj ||
				Stmt->Token == EX_LetBool || Stmt->Token == EX_LetDelegate ||
				Stmt->Token == EX_LetMulticastDelegate || Stmt->Token == EX_LetWeakObjPtr)
			{
				if (Stmt->Children.Num() >= 2 && Stmt->Children[0] && Stmt->Children[0]->PropertyRef)
				{
					uint8 DestToken = Stmt->Children[0]->Token;
					if (DestToken == EX_LocalVariable || DestToken == EX_InstanceVariable ||
						DestToken == EX_DefaultVariable)
					{
						FName DestName = Stmt->Children[0]->PropertyRef->GetFName();
						FString DestStr = DestName.ToString();
						if (!DestStr.Contains(TEXT("CallFunc_")) && !DestStr.Contains(TEXT("K2Node_")))
						{
							int32& Count = TempAssignCounts.FindOrAdd(DestName);
							Count++;
						}
					}
				}
			}
			// Also check EX_LetValueOnPersistentFrame (ubergraph assignments)
			if (Stmt->Token == EX_LetValueOnPersistentFrame && Stmt->PropertyRef)
			{
				FName DestName = Stmt->PropertyRef->GetFName();
				FString DestStr = DestName.ToString();
				if (!DestStr.Contains(TEXT("CallFunc_")) && !DestStr.Contains(TEXT("K2Node_")))
				{
					int32& Count = TempAssignCounts.FindOrAdd(DestName);
					Count++;
				}
			}
		}
		for (auto& Pair : TempAssignCounts)
		{
			if (Pair.Value > 1)
			{
				MultiAssignedTempVars.Add(Pair.Key);
			}
		}
	}

	// Pre-scan 2: detect stateful Temp_ variables (read-before-write).
	// Macros like FlipFlop, DoOnce, Gate compile to bytecode where a Temp_ variable
	// retains its value across calls. If the first READ occurs before the first WRITE
	// (by statement index), the variable is stateful — its initial value matters and
	// changes across invocations. These need TemporaryVariable + AssignmentStatement
	// nodes (same as multi-assigned vars) so the write compiles to real bytecode.
	// Without this, pin aliasing skips the write and the variable never updates.
	{
		TMap<FName, int32> TempFirstReadIdx;
		TMap<FName, int32> TempFirstWriteIdx;

		for (int32 i = StartIndex; i < EndIndex; i++)
		{
			TSharedPtr<FDecompiledExpr> Stmt = Statements[i];
			if (!Stmt) continue;
			if (pReachableStmts && !pReachableStmts->Contains(i)) continue;

			bool bIsLetVariant = (Stmt->Token == EX_Let || Stmt->Token == EX_LetObj ||
				Stmt->Token == EX_LetBool || Stmt->Token == EX_LetDelegate ||
				Stmt->Token == EX_LetMulticastDelegate || Stmt->Token == EX_LetWeakObjPtr);

			if (bIsLetVariant)
			{
				if (Stmt->Children.Num() >= 2 && Stmt->Children[0] && Stmt->Children[0]->PropertyRef)
				{
					// Track WRITE
					FName DestName = Stmt->Children[0]->PropertyRef->GetFName();
					if (DestName.ToString().StartsWith(TEXT("Temp_")))
					{
						if (!TempFirstWriteIdx.Contains(DestName))
						{
							TempFirstWriteIdx.Add(DestName, i);
						}
					}

					// Scan source expression (Children[1]) for READS — skip dest (Children[0])
					if (Stmt->Children.Num() > 1)
					{
						CollectTempVarReads(Stmt->Children[1], TempFirstReadIdx, i);
					}
				}
			}
			else if (Stmt->Token == EX_LetValueOnPersistentFrame && Stmt->PropertyRef)
			{
				// Track WRITE (dest is in PropertyRef, not Children)
				FName DestName = Stmt->PropertyRef->GetFName();
				if (DestName.ToString().StartsWith(TEXT("Temp_")))
				{
					if (!TempFirstWriteIdx.Contains(DestName))
					{
						TempFirstWriteIdx.Add(DestName, i);
					}
				}

				// Scan all children for READS (source expressions)
				for (auto& Child : Stmt->Children)
				{
					CollectTempVarReads(Child, TempFirstReadIdx, i);
				}
			}
			else
			{
				// Non-Let statements (JumpIfNot, function calls, etc.):
				// scan all children for READS
				for (auto& Child : Stmt->Children)
				{
					CollectTempVarReads(Child, TempFirstReadIdx, i);
				}
				if (Stmt->ContextObject)
				{
					CollectTempVarReads(Stmt->ContextObject, TempFirstReadIdx, i);
				}
			}
		}

		// Flag stateful temps: first read occurs before first write
		for (auto& Pair : TempFirstReadIdx)
		{
			if (MultiAssignedTempVars.Contains(Pair.Key))
				continue; // Already flagged by multi-assigned detection

			int32* WriteIdx = TempFirstWriteIdx.Find(Pair.Key);
			if (!WriteIdx || Pair.Value < *WriteIdx)
			{
				MultiAssignedTempVars.Add(Pair.Key);
				UE_LOG(LogGraphBuilder, Log, TEXT("Stateful Temp_ detected (read-before-write): %s (first read at stmt %d, first write at stmt %d)"),
					*Pair.Key.ToString(), Pair.Value, WriteIdx ? *WriteIdx : -1);
			}
		}
	}

	// ── Jump / flow tracking ──────────────────────────────────────────
	// OffsetToNode: maps bytecode start-offset → first graph node created there.
	// DeferredExecWires: exec pins that need to wire to a target bytecode offset
	//   after all nodes are created (forward jumps, branch false-paths).
	// FlowStack: simulated VM execution flow stack for Push/Pop pairs.
	// NextStatementOffset: chains each processed statement offset to the next one.
	// JumpForwardMap: maps EX_Jump offsets to their target offsets.
	//   Together these let deferred wire resolution follow execution chains through
	//   pure-only sections (EX_Let → Add_IntInt etc.) and jumps to find the nearest
	//   exec-bearing node (e.g., a Branch created by EX_PopExecutionFlowIfNot).
	TMap<int32, UEdGraphNode*> OffsetToNode;
	TArray<TPair<UEdGraphPin*, int32>> DeferredExecWires;
	TArray<int32> FlowStack;
	TMap<int32, int32> NextStatementOffset;
	TMap<int32, int32> JumpForwardMap;
	TSet<int32> ReturnOffsets; // Bytecode offsets of EX_Return statements
	int32 PrevProcessedOffset = -1;

	// ── Build processing order ──────────────────────────────────────────
	// When EventEntryStmtIdx is provided and is ABOVE StartIndex, backward jumps
	// have pulled the reachable set to include statements BEFORE the event entry.
	// Processing in raw index order would connect IncomingExecPin to loop body code
	// instead of the event entry, and break Push/Pop flow stack pairing.
	//
	// Fix: process event entry code first (gets IncomingExecPin), then backward-
	// jump targets second (only reached via deferred exec wires from EX_Jump).
	// A sentinel value of -1 marks the phase transition where PrevExecOut resets.
	TArray<int32> ProcessOrder;
	if (EventEntryStmtIdx >= 0 && EventEntryStmtIdx > StartIndex)
	{
		// Phase 1: event entry and forward
		for (int32 idx = EventEntryStmtIdx; idx < EndIndex; idx++)
			ProcessOrder.Add(idx);
		// Phase transition sentinel
		ProcessOrder.Add(-1);
		// Phase 2: backward-jump targets
		for (int32 idx = StartIndex; idx < EventEntryStmtIdx; idx++)
			ProcessOrder.Add(idx);
	}
	else
	{
		// No reordering needed (no backward-jump targets or no entry info)
		for (int32 idx = StartIndex; idx < EndIndex; idx++)
			ProcessOrder.Add(idx);
	}

	// ── Orphan PopExecutionFlow tracking ─────────────────────────────
	// In ForEach-style loops with backward-jump reordering, the loop body
	// continuation (at lower bytecode addresses, reached via Jump from the
	// body start) may contain a PopExecutionFlow that is processed BEFORE
	// its matching PushExecutionFlow. The Pop encounters an empty FlowStack
	// and becomes "orphaned." After the main loop, we match orphan Pops to
	// unmatched Push targets using address proximity to create the missing
	// exec wires (body end → increment). This post-processing approach avoids
	// cross-path interference between independent execution branches (e.g.,
	// custom mode vs normal mode loops sharing the same reachable statement range).
	TArray<TPair<UEdGraphPin*, int32>> OrphanFlowPops;  // (PrevExecOut, Pop bytecode address)
	TMap<int32, int32> PushTargetToAddr;                 // Push target → Push bytecode address

	for (int32 OrderIdx = 0; OrderIdx < ProcessOrder.Num(); OrderIdx++)
	{
		int32 i = ProcessOrder[OrderIdx];

		// Phase transition: backward-jump targets only reached via deferred exec wires
		if (i == -1)
		{
			PrevExecOut = nullptr;
			PrevProcessedOffset = -1;
			continue;
		}

		TSharedPtr<FDecompiledExpr> Stmt = Statements[i];
		if (!Stmt) continue;

		// If a reachable statement filter is provided, skip unreachable statements.
		// This prevents code from other events (reached via jumps from distant offsets)
		// from being incorrectly attributed to this event.
		if (pReachableStmts && !pReachableStmts->Contains(i))
		{
			continue;
		}

		// Skip terminators and no-ops
		if (Stmt->Token == EX_EndOfScript || Stmt->Token == EX_Nothing ||
			Stmt->Token == EX_Tracepoint || Stmt->Token == EX_WireTracepoint ||
			Stmt->Token == EX_InstrumentationEvent || Stmt->Token == EX_Breakpoint)
		{
			continue;
		}

		// ── Record statement offset chain ──
		// Build a sequential chain so deferred wire resolution can follow execution
		// through pure-only code sections (EX_Let → pure math) and jumps.
		if (PrevProcessedOffset >= 0)
		{
			NextStatementOffset.Add(PrevProcessedOffset, Stmt->StartOffset);
		}
		PrevProcessedOffset = Stmt->StartOffset;

		// ── EX_Jump: unconditional goto ──
		// Wire current exec chain to the jump target, then break linear flow.
		if (Stmt->Token == EX_Jump)
		{
			JumpForwardMap.Add(Stmt->StartOffset, Stmt->JumpTarget);
			if (PrevExecOut && Stmt->JumpTarget != 0)
			{
				DeferredExecWires.Add(TPair<UEdGraphPin*, int32>(PrevExecOut, Stmt->JumpTarget));
			}
			PrevExecOut = nullptr; // Break linear chain — next statement needs exec from elsewhere
			PrevProcessedOffset = -1; // Break offset chain — jump target is non-sequential
			continue;
		}

		// ── EX_PushExecutionFlow: push jump target onto flow stack ──
		if (Stmt->Token == EX_PushExecutionFlow)
		{
			FlowStack.Push(Stmt->JumpTarget);
			// Record Push target → Push address for post-processing orphan Pop matching
			PushTargetToAddr.Add(Stmt->JumpTarget, Stmt->StartOffset);
			continue;
		}

		// ── EX_PopExecutionFlow: pop flow stack, wire exec to the saved address ──
		if (Stmt->Token == EX_PopExecutionFlow)
		{
			if (FlowStack.Num() > 0)
			{
				int32 PopTarget = FlowStack.Pop(/*bAllowShrinking=*/false);
				if (PrevExecOut)
				{
					DeferredExecWires.Add(TPair<UEdGraphPin*, int32>(PrevExecOut, PopTarget));
				}
			}
			else if (PrevExecOut)
			{
				// FlowStack empty — the matching Push hasn't been processed yet
				// (backward-jump reordering). Record as orphan Pop for post-processing.
				OrphanFlowPops.Add(TPair<UEdGraphPin*, int32>(PrevExecOut, Stmt->StartOffset));
				UE_LOG(LogGraphBuilder, Log, TEXT("  Orphan PopExecutionFlow at offset %d (FlowStack empty, recorded for post-processing)"),
					Stmt->StartOffset);
			}
			PrevExecOut = nullptr; // Execution resumes at the popped address
			PrevProcessedOffset = -1; // Break offset chain — execution resumes elsewhere
			continue;
		}

		// ── EX_Return: function exit ──
		// Collect the exec pin leading to this return so BuildFunctionGraph can
		// wire ALL return paths to the FunctionResult node (not just the last one).
		// EX_Return is a terminal statement — break the linear exec chain after it.
		if (Stmt->Token == EX_Return)
		{
			ReturnOffsets.Add(Stmt->StartOffset);

			// Record the exec pin that reaches this return
			if (PrevExecOut)
			{
				ReturnExecSources.AddUnique(PrevExecOut);
			}

			// Process return value if present
			if (Stmt->Children.Num() > 0 && Stmt->Children[0])
			{
				if (Stmt->Children[0]->Token != EX_Nothing)
				{
					// Return with an actual value — resolve it and record as ReturnValue
					TSharedPtr<FDecompiledExpr> RetExpr = Stmt->Children[0];
					if (RetExpr->Token == EX_LocalOutVariable && RetExpr->PropertyRef)
					{
						// Already a local out variable — should be in the map from a previous EX_Let
						// No need to do anything extra, the FunctionResult node creation will find it
					}
					else if (RetExpr->IsLiteral())
					{
						// Direct return of a literal — store for FunctionResult pin default
						OutParamLiteralMap.Add(UEdGraphSchema_K2::PN_ReturnValue, RetExpr);
						UE_LOG(LogGraphBuilder, Log, TEXT("EX_Return ← literal (token=0x%02X), stored as ReturnValue"),
							(uint8)RetExpr->Token);
					}
					else
					{
						// Direct return of an expression — resolve and record
						int32 DataX = CurrentX - 200;
						int32 DataY = CurrentY + DataNodeOffsetY;
						UEdGraphPin* RetPin = ResolveDataExpr(Graph, RetExpr, DataX, DataY);
						if (RetPin)
						{
							LocalVarToPinMap.Add(UEdGraphSchema_K2::PN_ReturnValue, RetPin);
						}
					}
				}
			}

			PrevExecOut = nullptr; // Break chain — return exits the function
			continue;
		}

		// Create a node for this statement
		UEdGraphNode* NewNode = CreateNodeForExpr(Graph, Stmt, CurrentX, CurrentY);

		if (NewNode)
		{
			// Record offset → node for deferred jump wiring
			if (!OffsetToNode.Contains(Stmt->StartOffset))
			{
				OffsetToNode.Add(Stmt->StartOffset, NewNode);
			}

			// Wire execution flow from previous node
			if (PrevExecOut)
			{
				UEdGraphPin* NodeExecIn = FindExecPin(NewNode, EGPD_Input);
				TryConnect(PrevExecOut, NodeExecIn);
			}

			// Update the exec chain — find the appropriate exec output pin
			UEdGraphPin* NodeExecOut = nullptr;

			if (Stmt->Token == EX_JumpIfNot)
			{
				// Branch node: True pin continues linearly
				NodeExecOut = FindExecPin(NewNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);

				// False pin wires to the explicit jump target
				UEdGraphPin* ElsePin = FindExecPin(NewNode, EGPD_Output, UEdGraphSchema_K2::PN_Else);
				if (ElsePin && Stmt->JumpTarget != 0)
				{
					DeferredExecWires.Add(TPair<UEdGraphPin*, int32>(ElsePin, Stmt->JumpTarget));
				}
			}
			else if (Stmt->Token == EX_PopExecutionFlowIfNot)
			{
				// Branch node: True pin continues linearly (condition passed)
				NodeExecOut = FindExecPin(NewNode, EGPD_Output, UEdGraphSchema_K2::PN_Then);

				// False pin: wire to flow stack top (condition failed → pop and jump).
				// IMPORTANT: PEEK (don't pop) the flow stack. In the real VM, PopExecutionFlowIfNot
				// only consumes the stack entry when the condition is FALSE. When TRUE, the entry
				// stays for a subsequent PopExecutionFlow on the Then path. Since we're building
				// the graph for both paths simultaneously, we must keep the entry available.
				// The Else path is fully modeled by the deferred wire; the Then path needs the
				// entry for later consumption (e.g., after Array_Add → PopExecutionFlow in a loop).
				UEdGraphPin* ElsePin = FindExecPin(NewNode, EGPD_Output, UEdGraphSchema_K2::PN_Else);
				if (ElsePin && FlowStack.Num() > 0)
				{
					int32 PopTarget = FlowStack.Last();
					DeferredExecWires.Add(TPair<UEdGraphPin*, int32>(ElsePin, PopTarget));
				}
			}
			else
			{
				// Regular node: find the first exec output
				NodeExecOut = FindExecPin(NewNode, EGPD_Output);
			}

			if (NodeExecOut)
			{
				PrevExecOut = NodeExecOut;
			}

			// ── Latent function resume wiring ──────────────────────────────
			// Latent functions (Delay, MoveComponentTo, Timeline, etc.) store a
			// resume offset in their LatentActionInfo struct argument. The engine
			// calls back into the ubergraph at that offset when the latent action
			// completes. Wire the node's exec output ("Completed"/"Then") to the
			// resume offset, and break the linear exec chain — the PopExecutionFlow
			// that follows the latent call is just the VM returning to the calling
			// context, not a graph connection.
			if (NewNode && PrevExecOut)
			{
				int32 LatentResumeOffset = -1;
				for (const TSharedPtr<FDecompiledExpr>& Child : Stmt->Children)
				{
					if (Child && Child->Token == EX_StructConst && Child->StructRef &&
						Child->StructRef->GetName() == TEXT("LatentActionInfo"))
					{
						for (const TSharedPtr<FDecompiledExpr>& StructChild : Child->Children)
						{
							if (StructChild && StructChild->Token == EX_SkipOffsetConst)
							{
								LatentResumeOffset = StructChild->JumpTarget;
								break;
							}
						}
						break;
					}
				}

				if (LatentResumeOffset > 0)
				{
					DeferredExecWires.Add(TPair<UEdGraphPin*, int32>(PrevExecOut, LatentResumeOffset));
					PrevExecOut = nullptr;
					PrevProcessedOffset = -1;
					UE_LOG(LogGraphBuilder, Log, TEXT("Latent function at offset %d, wiring Completed → resume offset %d"),
						Stmt->StartOffset, LatentResumeOffset);
				}
			}

			CurrentX += NodeSpacingX;
		}
	}

	// ── Post-processing: match orphan Pops to leftover FlowStack targets ──
	// In ForEach-style loops with backward-jump reordering, Pop at the end of
	// the loop body (lower bytecode address) is processed before the Push in the
	// loop header (higher address). The Pop finds an empty FlowStack and becomes
	// "orphaned." Meanwhile, the Push adds its target to FlowStack but it's never
	// popped (since the Pop already ran). We match them by address proximity:
	// for each leftover FlowStack target, find the orphan Pop with the highest
	// bytecode address that is still below the Push address for that target.
	if (OrphanFlowPops.Num() > 0 && FlowStack.Num() > 0)
	{
		UE_LOG(LogGraphBuilder, Log, TEXT("Post-processing: %d orphan Pops, %d leftover FlowStack targets"),
			OrphanFlowPops.Num(), FlowStack.Num());

		// Process leftover FlowStack targets (iterate copy since we modify FlowStack)
		TArray<int32> LeftoverTargets;
		while (FlowStack.Num() > 0)
		{
			LeftoverTargets.Add(FlowStack.Pop(/*bAllowShrinking=*/false));
		}

		for (int32 Target : LeftoverTargets)
		{
			int32* PushAddrPtr = PushTargetToAddr.Find(Target);
			if (!PushAddrPtr) continue;
			int32 PushAddr = *PushAddrPtr;

			// Find the orphan Pop with the highest bytecode address below PushAddr.
			// This is the Pop that belongs to this Push's loop body continuation.
			int32 BestIdx = -1;
			int32 BestAddr = -1;
			for (int32 j = 0; j < OrphanFlowPops.Num(); j++)
			{
				int32 PopAddr = OrphanFlowPops[j].Value;
				if (PopAddr < PushAddr && PopAddr > BestAddr)
				{
					BestAddr = PopAddr;
					BestIdx = j;
				}
			}

			if (BestIdx >= 0)
			{
				UEdGraphPin* OrphanExecOut = OrphanFlowPops[BestIdx].Key;
				DeferredExecWires.Add(TPair<UEdGraphPin*, int32>(OrphanExecOut, Target));
				UE_LOG(LogGraphBuilder, Log, TEXT("  Matched orphan Pop@%d → target %d (Push@%d)"),
					OrphanFlowPops[BestIdx].Value, Target, PushAddr);
				OrphanFlowPops.RemoveAt(BestIdx);
			}
			else
			{
				UE_LOG(LogGraphBuilder, Warning, TEXT("  No orphan Pop found for leftover target %d (Push@%d)"),
					Target, PushAddr);
			}
		}

		// Log any remaining unmatched orphan Pops
		for (const auto& Orphan : OrphanFlowPops)
		{
			UE_LOG(LogGraphBuilder, Log, TEXT("  Unmatched orphan Pop@%d (no corresponding Push target)"),
				Orphan.Value);
		}
	}

	// ── Resolve deferred exec wires ──────────────────────────────────
	// For each pending connection, follow the statement offset chain from the
	// target bytecode offset to find the nearest exec-bearing node.
	// This handles cases where the target offset lands in a pure-only section
	// (e.g., EX_Let → Add_IntInt increment) that chains through jumps to
	// eventually reach an exec node (e.g., a Branch from EX_PopExecutionFlowIfNot).
	for (const auto& Wire : DeferredExecWires)
	{
		UEdGraphPin* SourcePin = Wire.Key;
		int32 TargetOffset = Wire.Value;

		if (!SourcePin || SourcePin->LinkedTo.Num() > 0) continue;

		// Try exact offset match first — cheapest check
		if (UEdGraphNode** ExactNode = OffsetToNode.Find(TargetOffset))
		{
			UEdGraphPin* ExecIn = FindExecPin(*ExactNode, EGPD_Input);
			if (ExecIn)
			{
				TryConnect(SourcePin, ExecIn);
				continue;
			}
		}

		// Follow the execution chain through NextStatementOffset and JumpForwardMap
		// to find the correct exec-bearing node. This is more accurate than
		// "nearest node >= target" because backward jumps (loops) can target offsets
		// whose correct exec node is at a LOWER offset via the jump chain.
		// E.g., PopExecutionFlow targets loop increment at 15841, which chains
		// through EX_Jump → backward to loop condition Branch at 15523.
		TSet<int32> Visited;
		int32 CurrentOffset = TargetOffset;
		bool bConnected = false;

		while (!bConnected && !Visited.Contains(CurrentOffset))
		{
			Visited.Add(CurrentOffset);

			// Check if there's an exec-bearing node at this offset
			if (UEdGraphNode** NodePtr = OffsetToNode.Find(CurrentOffset))
			{
				UEdGraphPin* ExecIn = FindExecPin(*NodePtr, EGPD_Input);
				if (ExecIn)
				{
					TryConnect(SourcePin, ExecIn);
					bConnected = true;
					break;
				}
			}

			// Follow jump if this offset has one
			if (int32* JumpTarget = JumpForwardMap.Find(CurrentOffset))
			{
				CurrentOffset = *JumpTarget;
				continue;
			}

			// Follow sequential chain to next statement
			if (int32* NextOffset = NextStatementOffset.Find(CurrentOffset))
			{
				CurrentOffset = *NextOffset;
				continue;
			}

			break; // Dead end — no more statements to follow
		}

		// Fallback: if the chain walk didn't find anything, try nearest node >= target.
		// This handles cases where the NextStatementOffset chain has gaps.
		if (!bConnected)
		{
			UEdGraphNode* TargetNode = nullptr;
			int32 BestOffset = INT32_MAX;
			for (const auto& Entry : OffsetToNode)
			{
				if (Entry.Key >= TargetOffset && Entry.Key < BestOffset)
				{
					BestOffset = Entry.Key;
					TargetNode = Entry.Value;
				}
			}

			if (TargetNode)
			{
				UEdGraphPin* TargetExecIn = FindExecPin(TargetNode, EGPD_Input);
				if (TargetExecIn)
				{
					TryConnect(SourcePin, TargetExecIn);
				}
			}
		}
	}

	// ── Deferred data wire resolution ──────────────────────────────────
	// Backward-jump bytecode patterns (e.g., init code at high offsets jumping
	// back to a loop at low offsets) cause consumer statements to be processed
	// before producer statements. The producers populate LocalVarToPinMap after
	// the consumers have already tried (and failed) to resolve the variables.
	// Resolve these deferred connections now that all statements are processed.
	for (const auto& Wire : DeferredDataWires)
	{
		UEdGraphPin* InputPin = Wire.Key;
		FName VarName = Wire.Value;

		if (!InputPin || InputPin->LinkedTo.Num() > 0) continue; // Already connected

		if (UEdGraphPin** FoundPin = LocalVarToPinMap.Find(VarName))
		{
			if (*FoundPin)
			{
				ForceConnect(*FoundPin, InputPin);
				UE_LOG(LogGraphBuilder, Log, TEXT("DeferredDataWire: %s.%s → %s.%s (var=%s)"),
					*(*FoundPin)->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*(*FoundPin)->PinName.ToString(),
					*InputPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*InputPin->PinName.ToString(),
					*VarName.ToString());
			}
		}
		else if (UEdGraphPin** PersistPin = PersistentFrameVarMap.Find(VarName))
		{
			if (*PersistPin)
			{
				ForceConnect(*PersistPin, InputPin);
				UE_LOG(LogGraphBuilder, Log, TEXT("DeferredDataWire (cross-event): %s.%s → %s.%s (var=%s)"),
					*(*PersistPin)->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*(*PersistPin)->PinName.ToString(),
					*InputPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*InputPin->PinName.ToString(),
					*VarName.ToString());
			}
		}
		else
		{
			UE_LOG(LogGraphBuilder, Warning, TEXT("DeferredDataWire UNRESOLVED: %s.%s needs var '%s' (not in LocalVarToPinMap)"),
				*InputPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*InputPin->PinName.ToString(),
				*VarName.ToString());
		}
	}

	// ── Deferred exec wires targeting EX_Return ─────────────────────────
	// Branch Else pins that defer to an EX_Return offset can't find a target
	// node (EX_Return doesn't create one). These paths should also connect to
	// the FunctionResult node. Walk the execution chain from unresolved deferred
	// wires to see if they ultimately reach an EX_Return.
	for (const auto& Wire : DeferredExecWires)
	{
		UEdGraphPin* SourcePin = Wire.Key;
		int32 TargetOffset = Wire.Value;

		if (!SourcePin || SourcePin->LinkedTo.Num() > 0) continue; // Already resolved

		// Walk the chain to see if target leads to an EX_Return
		TSet<int32> Visited;
		int32 CurrentOffset = TargetOffset;
		while (!Visited.Contains(CurrentOffset))
		{
			Visited.Add(CurrentOffset);
			if (ReturnOffsets.Contains(CurrentOffset))
			{
				ReturnExecSources.AddUnique(SourcePin);
				break;
			}
			if (int32* JumpTarget = JumpForwardMap.Find(CurrentOffset))
			{
				CurrentOffset = *JumpTarget;
				continue;
			}
			if (int32* NextOffset = NextStatementOffset.Find(CurrentOffset))
			{
				CurrentOffset = *NextOffset;
				continue;
			}
			break;
		}
	}

	// Persist the last exec output so BuildFunctionGraph can wire FunctionResult
	LastFunctionExecOut = PrevExecOut;
}

// Node Creation from Expressions

UEdGraphNode* FGraphBuilder::CreateNodeForExpr(
	UEdGraph* Graph,
	TSharedPtr<FDecompiledExpr> Expr,
	int32& X, int32& Y)
{
	if (!Expr || !Graph) return nullptr;

	switch (Expr->Token)
	{
		// FUNCTION CALLS → UK2Node_CallFunction
	case EX_FinalFunction:
	case EX_LocalFinalFunction:
	case EX_CallMath:
	{
		UFunction* TargetFunc = Expr->FunctionRef;
		if (!TargetFunc)
		{
			Warn(FString::Printf(TEXT("Null function reference at offset 0x%04X"), Expr->StartOffset));
			return nullptr;
		}

		// Don't create nodes for pure functions called as sub-expressions
		// (they'll be created by ResolveDataExpr when wiring to consuming nodes)
		// But DO create them as top-level statements
		// Since we're called from EmitStatements, we always create the node here

		// ── Parent call detection ─────────────────────────────────────
		// Detect Super:: calls to parent implementations of BlueprintEvents.
		// In bytecode, these appear as EX_FinalFunction/EX_LocalFinalFunction
		// targeting a parent class's compiled function.
		//
		// CRITICAL: Only detect parent calls for ORIGINAL EX_FinalFunction tokens.
		// EX_VirtualFunction calls are virtual dispatch (never Super::) — they get
		// redirected to this handler but must NOT become CallParentFunction.
		//
		// CHILD CLASS MODE: Super:: calls are SKIPPED entirely.
		// In the child class, the decompiled logic IS the original class's implementation.
		// CallParentFunction would call the cooked parent (which has the same logic),
		// causing double execution. The exec chain bridges over the skipped call.
		//
		// Detection strategy:
		// The bytecode may target a BPGC function (e.g., FirstPersonCharacter_C::ReceiveTick)
		// whose compiled version doesn't carry FUNC_BlueprintEvent. We need to check if
		// the function NAME corresponds to a BlueprintEvent declared in any NATIVE ancestor
		// (e.g., AActor::ReceiveTick). If so, a direct (non-virtual) call to a parent
		// class's version of that function is a Super:: call.
		bool bIsParentCall = false;
		if (!bIsRedirectedVirtualCall && !Expr->ContextObject)
		{
			UClass* FuncOwner = TargetFunc->GetOwnerClass();
			if (FuncOwner && CurrentOriginalClass &&
				FuncOwner != CurrentOriginalClass &&
				CurrentOriginalClass->IsChildOf(FuncOwner))
			{
				// Target is on a parent class. Check if the function name corresponds
				// to a BlueprintEvent anywhere in the native class hierarchy.
				FName FuncName = TargetFunc->GetFName();

				// Check the target function itself first
				if (TargetFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					bIsParentCall = true;
				}
				else
				{
					// Walk up the NATIVE class hierarchy looking for a matching
					// BlueprintImplementableEvent/BlueprintNativeEvent declaration
					for (UClass* Check = CurrentOriginalClass; Check; Check = Check->GetSuperClass())
					{
						if (Cast<UBlueprintGeneratedClass>(Check) != nullptr)
							continue; // Skip BPGCs, look for native declarations

						UFunction* NativeFunc = Check->FindFunctionByName(FuncName, EIncludeSuperFlag::ExcludeSuper);
						if (NativeFunc && NativeFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
						{
							bIsParentCall = true;
							break;
						}
					}
				}
			}
		}

		if (bIsParentCall)
		{
			// K2Node_CallParentFunction needs its FunctionReference set via
			// SetExternalMember pointing to the Blueprint's parent class, just like
			// the UE4 editor does when you "Add call to parent function".
			// Using SetFromFunction with the BPGC's compiled function causes
			// "invalid function signature" warnings.

			// Find the Blueprint we're building
			UBlueprint* OwnerBP = Cast<UBlueprint>(Graph->GetOuter());
			UClass* ParentClass = OwnerBP ? OwnerBP->ParentClass : nullptr;
			if (!ParentClass && CurrentOriginalClass)
			{
				ParentClass = CurrentOriginalClass->GetSuperClass();
			}

			// Walk up to find the native declaration for signature (pins)
			UFunction* NativeOriginFunc = TargetFunc;
			while (NativeOriginFunc->GetSuperFunction())
			{
				NativeOriginFunc = NativeOriginFunc->GetSuperFunction();
			}

			UK2Node_CallParentFunction* ParentCallNode = NewObject<UK2Node_CallParentFunction>(Graph);
			ParentCallNode->CreateNewGuid();

			// Set FunctionReference to the NATIVE origin function (e.g., AActor::ReceiveTick).
			// Using the BPGC parent (FirstPersonCharacter_C::ReceiveTick) causes
			// "invalid function signature" because the BPGC's compiled function may
			// have a different signature than the native BlueprintEvent declaration.
			ParentCallNode->FunctionReference.SetExternalMember(
				NativeOriginFunc->GetFName(),
				NativeOriginFunc->GetOwnerClass());

			ParentCallNode->NodePosX = X;
			ParentCallNode->NodePosY = Y;
			ParentCallNode->AllocateDefaultPins();
			Graph->AddNode(ParentCallNode, false, false);

			UE_LOG(LogGraphBuilder, Log, TEXT("Created CallParentFunction for %s → parent %s"),
				*TargetFunc->GetName(),
				ParentClass ? *ParentClass->GetName() : TEXT("(native)"));

			// Wire argument data pins (same logic as regular function calls)
			int32 DataX = X - 200;
			int32 DataY = Y + DataNodeOffsetY;

			int32 ArgIndex = 0;
			for (const TSharedPtr<FDecompiledExpr>& ArgExpr : Expr->Children)
			{
				if (!ArgExpr) { ArgIndex++; continue; }

				FName ParamPinName = GetPinNameForFunctionParam(TargetFunc, ArgIndex);
				if (ParamPinName != NAME_None)
				{
					FString PinNameStr = ParamPinName.ToString();
					if (PinNameStr.Contains(TEXT("LatentInfo")) || PinNameStr == TEXT("LatentActionInfo"))
					{
						ArgIndex++;
						continue;
					}
				}

				// Check if this argument is for an out parameter — if so, map the
				// local variable to the call node's OUTPUT pin instead of wiring as input.
				if (ParamPinName != NAME_None && ArgExpr->PropertyRef)
				{
					FProperty* ParamProp = nullptr;
					for (TFieldIterator<FProperty> It(TargetFunc); It; ++It)
					{
						if (It->GetFName() == ParamPinName && It->HasAnyPropertyFlags(CPF_Parm))
						{
							ParamProp = *It;
							break;
						}
					}

					if (ParamProp && ParamProp->HasAnyPropertyFlags(CPF_OutParm) &&
						!ParamProp->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						UEdGraphPin* OutPin = FindDataPin(ParentCallNode, EGPD_Output, ParamPinName);
						if (OutPin)
						{
							FName VarName = ArgExpr->PropertyRef->GetFName();
							LocalVarToPinMap.Add(VarName, OutPin);
							ArgIndex++;
							continue;
						}
					}
				}

				UEdGraphPin* DataPin = ResolveDataExpr(Graph, ArgExpr, DataX, DataY);
				if (DataPin && ParamPinName != NAME_None)
				{
					UEdGraphPin* TargetPin = FindDataPin(ParentCallNode, EGPD_Input, ParamPinName);
					if (TargetPin)
					{
						ForceConnect(DataPin, TargetPin);
					}
				}
				ArgIndex++;
				DataY += DataNodeOffsetY;
			}

			X += NodeSpacingX;
			return ParentCallNode;
		}

		// Use UK2Node_CallArrayFunction for KismetArrayLibrary functions.
		// This subclass overrides PinConnectionListChanged to propagate array
		// element types from connected pins, fixing wildcard pin issues on
		// Array_Get, Array_Length, Array_Add, etc.
		bool bIsArrayLibraryFunc = false;
		{
			UClass* FuncClass = TargetFunc->GetOwnerClass();
			if (FuncClass && FuncClass->GetName().Contains(TEXT("KismetArrayLibrary")))
			{
				bIsArrayLibraryFunc = true;
			}
		}
		UK2Node_CallFunction* CallNode = bIsArrayLibraryFunc
			? NewObject<UK2Node_CallArrayFunction>(Graph)
			: NewObject<UK2Node_CallFunction>(Graph);
		CallNode->CreateNewGuid();

		// Check if this function is from a BlueprintGeneratedClass.
		// Only redirect to self-member if the function belongs to the SAME class
		// hierarchy as our blueprint (i.e., it's defined on this class or a parent).
		// Functions on OTHER BPGCs (e.g., FNAFInventorySystem::HasItem called via
		// GetGameInstanceSubsystem) must keep their original function reference.
		UClass* FuncOwner = TargetFunc->GetOwnerClass();
		bool bIsBPGCFunc = FuncOwner && Cast<UBlueprintGeneratedClass>(FuncOwner) != nullptr;
		bool bIsSelfBPGCFunc = bIsBPGCFunc && CurrentOriginalClass && CurrentOriginalClass->IsChildOf(FuncOwner);

		// Also check if the function owner is an interface that this class implements.
		// Interface functions called on self use virtual dispatch (EX_VirtualFunction)
		// without a context object. IsChildOf doesn't cover interfaces, so check
		// ImplementsInterface separately. Also check the original class's Interfaces array
		// for BPI_ classes that IsChildOf/ImplementsInterface might miss on cooked classes.
		if (!bIsSelfBPGCFunc && bIsBPGCFunc && CurrentOriginalClass && !Expr->ContextObject)
		{
			if (CurrentOriginalClass->ImplementsInterface(FuncOwner))
			{
				bIsSelfBPGCFunc = true;
			}
			else
			{
				// Check the Interfaces array directly (cooked BPGCs may not report
				// ImplementsInterface correctly for BP interfaces)
				for (const FImplementedInterface& Iface : CurrentOriginalClass->Interfaces)
				{
					if (Iface.Class == FuncOwner ||
						(Iface.Class && Iface.Class->GetFName() == FuncOwner->GetFName()))
					{
						bIsSelfBPGCFunc = true;
						break;
					}
				}
			}
		}

		// AVOID SetFromFunction entirely — it calls SetGivenSelfScope which
		// dereferences GetOwnerClass() without null check, crashing for delegate
		// signature functions on cooked BPGCs. Instead, set function properties
		// and reference manually.
		CallNode->bIsPureFunc = TargetFunc->HasAnyFunctionFlags(FUNC_BlueprintPure);
		CallNode->bIsConstFunc = TargetFunc->HasAnyFunctionFlags(FUNC_Const);
		// Note: DetermineWantsEnumToExecExpansion is private — skip it.
		// It only affects byte-enum-to-exec-pin expansion, which is uncommon.

		if (bIsSelfBPGCFunc)
		{
			// Self-member call — function is defined on this class or a parent BPGC.
			// Use SetExternalMember pointing to the actual function owner for
			// AllocateDefaultPins so GetTargetFunction can resolve the function.
			// In full uncook mode, SetSelfMember fails because the new BP's
			// GeneratedClass doesn't have the function yet — ResolveMember searches
			// the new class hierarchy which excludes OriginalClass.
			// After pins are created, switch to SetSelfMember for compilation.
			CallNode->FunctionReference.SetExternalMember(TargetFunc->GetFName(), FuncOwner);
		}
		else
		{
			// External function — set reference with owner class.
			// FuncOwner may be null for delegate signatures outered to the package.
			UClass* RefClass = FuncOwner ? FuncOwner : CurrentOriginalClass;
			CallNode->FunctionReference.SetExternalMember(TargetFunc->GetFName(), RefClass);
		}

		CallNode->NodePosX = X;
		CallNode->NodePosY = Y;
		CallNode->AllocateDefaultPins();
		FixStructPinDefaults(CallNode);
		Graph->AddNode(CallNode, false, false);

		if (bIsSelfBPGCFunc)
		{
			// Now switch to self-member for correct compilation semantics.
			// The compiler needs SetSelfMember so it resolves against the newly
			// compiled class, not the original cooked class.
			CallNode->FunctionReference.SetSelfMember(TargetFunc->GetFName());

			// Remove the Target/Self pin — self-member calls don't need it.
			// AllocateDefaultPins created this pin because we used SetExternalMember.
			UEdGraphPin* SelfPin = FindDataPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
			if (SelfPin)
			{
				CallNode->Pins.Remove(SelfPin);
				SelfPin->MarkPendingKill();
			}
		}

		// Wire argument data pins
		int32 DataX = X - 200;
		int32 DataY = Y + DataNodeOffsetY;

		int32 ArgIndex = 0;
		for (const TSharedPtr<FDecompiledExpr>& ArgExpr : Expr->Children)
		{
			if (!ArgExpr) { ArgIndex++; continue; }

			// Check if this argument is for a LatentInfo parameter — latent functions
			// auto-populate this from the compiler; we should never create nodes for it.
			FName ParamPinName = GetPinNameForFunctionParam(TargetFunc, ArgIndex);
			if (ParamPinName != NAME_None)
			{
				FString PinNameStr = ParamPinName.ToString();
				if (PinNameStr.Contains(TEXT("LatentInfo")) || PinNameStr == TEXT("LatentActionInfo"))
				{
					ArgIndex++;
					continue;
				}
			}

			// Check if this argument is for an out parameter — if so, map the
			// local variable to the call node's OUTPUT pin instead of wiring as input.
			// This handles patterns like: SpawnJumpscarePawn(..., EX_LocalOutVariable(Temp_xxx))
			// where Temp_xxx needs to map to the "Jumpscare Pawn Out" output pin.
			// ParamPinName already declared above for LatentInfo check
			if (ParamPinName != NAME_None && ArgExpr->PropertyRef)
			{
				// Look up the UFunction property to check for CPF_OutParm
				FProperty* ParamProp = nullptr;
				for (TFieldIterator<FProperty> It(TargetFunc); It; ++It)
				{
					if (It->GetFName() == ParamPinName && It->HasAnyPropertyFlags(CPF_Parm))
					{
						ParamProp = *It;
						break;
					}
				}

				if (ParamProp && ParamProp->HasAnyPropertyFlags(CPF_OutParm) &&
					!ParamProp->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					// Check if the CALL NODE has an output pin for this parameter.
					// True output params (like SpawnJumpscarePawn's JumpscarePawnOut) have
					// output pins. By-ref input params (like SpawnTransform) only have input pins.
					UEdGraphPin* OutPin = FindDataPin(CallNode, EGPD_Output, ParamPinName);
					if (OutPin)
					{
						// True output parameter — map variable to output pin, skip input wiring
						FName VarName = ArgExpr->PropertyRef->GetFName();
						LocalVarToPinMap.Add(VarName, OutPin);
						ArgIndex++;
						continue;
					}
					// No output pin → this is a by-ref INPUT param (like SpawnTransform)
					// Fall through to normal input wiring below
				}
			}

			// Find the corresponding input pin on the call node
			FName PinName = ParamPinName;
			UEdGraphPin* InputPin = nullptr;

			if (PinName != NAME_None)
			{
				InputPin = FindDataPin(CallNode, EGPD_Input, PinName);
			}

			// Fallback: try to find pin by index among input data pins
			if (!InputPin)
			{
				int32 DataPinIdx = 0;
				for (UEdGraphPin* Pin : CallNode->Pins)
				{
					if (Pin->Direction == EGPD_Input &&
						Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
						Pin->PinName != UEdGraphSchema_K2::PN_Self)
					{
						if (DataPinIdx == ArgIndex)
						{
							InputPin = Pin;
							break;
						}
						DataPinIdx++;
					}
				}
			}

			if (InputPin)
			{
				// Handle context (target pin) from EX_Context wrapping
				// Skip EX_ObjectConst — world context objects are auto-provided
				if (Expr->ContextObject && ArgIndex == 0 &&
					Expr->ContextObject->Token != EX_ObjectConst)
				{
					// The context object goes to the Self/Target pin
					UEdGraphPin* TargetPin = FindDataPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
					if (TargetPin)
					{
						UEdGraphPin* ContextOutPin = ResolveDataExpr(Graph, Expr->ContextObject, DataX, DataY);
						// Use ForceConnect — bytecode context chains are known-correct
						ForceConnect(ContextOutPin, TargetPin);
						DataY += NodeSpacingY / 2;
					}
				}

				// Resolve the argument expression to a pin
				if (ArgExpr->IsLiteral())
				{
					// Smart pin default handling based on token type and pin type
					switch (ArgExpr->Token)
					{
					case EX_ObjectConst:
						if (ArgExpr->ObjectRef)
						{
							// For class pins, set as class reference
							if (InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
								InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
							{
								InputPin->DefaultObject = ArgExpr->ObjectRef;
							}
							else
							{
								// For object pins, use DefaultObject
								InputPin->DefaultObject = ArgExpr->ObjectRef;
							}
						}
						break;

					case EX_NoObject:
						// Null object — leave pin at its default (empty)
						// Do NOT set DefaultValue = "None" as that's invalid for object pins
						InputPin->DefaultValue = TEXT("");
						break;

					case EX_Self:
					{
						// For hidden pins (WorldContextObject etc.), the BP compiler
						// auto-fills self — no node needed. For visible pins (Owner,
						// TestObject, InPawn, etc.) self must be explicitly wired.
						if (!InputPin->bHidden)
						{
							UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
							SelfNode->CreateNewGuid();
							SelfNode->NodePosX = DataX;
							SelfNode->NodePosY = DataY;
							SelfNode->AllocateDefaultPins();
							Graph->AddNode(SelfNode, false, false);
							UEdGraphPin* SelfOut = FindFirstDataOutputPin(SelfNode);
							TryConnect(SelfOut, InputPin);
							DataX -= NodeSpacingX / 2;
							DataY += NodeSpacingY / 2;
						}
						break;
					}

					case EX_ByteConst:
					case EX_IntConst:
					case EX_IntConstByte:
					case EX_IntZero:
					case EX_IntOne:
					{
						// Check if this pin expects an enum — if so, look up the enum name
						int64 EnumVal = 0;
						if (ArgExpr->Token == EX_ByteConst)
							EnumVal = ArgExpr->ByteValue;
						else
							EnumVal = ArgExpr->IntValue;
						if (InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
							InputPin->PinType.PinSubCategoryObject.IsValid())
						{
							UEnum* PinEnum = Cast<UEnum>(InputPin->PinType.PinSubCategoryObject.Get());
							if (PinEnum)
							{
								FString EnumName = PinEnum->GetNameStringByValue(EnumVal);
								if (!EnumName.IsEmpty())
								{
									InputPin->DefaultValue = EnumName;
									break;
								}
							}
						}
						// Fall through to string default for non-enum int/byte pins
						InputPin->DefaultValue = ArgExpr->GetLiteralAsString();
						break;
					}

					default:
					{
						FString DefaultVal = ArgExpr->GetLiteralAsString();
						// Don't set "None" as default for object/class/softobject/softclass pins —
						// the string "None" is invalid; those pins expect empty or a DefaultObject.
						bool bIsObjectPin = InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
							InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
							InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
							InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass ||
							InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface;
						if (!DefaultVal.IsEmpty() && !(bIsObjectPin && DefaultVal == TEXT("None")))
						{
							InputPin->DefaultValue = DefaultVal;
						}
						break;
					}
					}
				}
				else
				{
					// Check if this is a local variable referencing a stored Temp_ literal
					// (e.g., compiler inlined a class/object selection on the node's dropdown)
					if (ApplyTempLiteralToPin(ArgExpr, InputPin))
					{
						// Literal was applied as pin default — done
					}
					else
					{
						// Create a node for the sub-expression and wire it
						UEdGraphPin* ArgOutPin = ResolveDataExpr(Graph, ArgExpr, DataX, DataY);
						if (ArgOutPin)
						{
							TryConnect(ArgOutPin, InputPin);
						}
						else if (ArgExpr->PropertyRef &&
							(ArgExpr->Token == EX_LocalVariable || ArgExpr->Token == EX_LocalOutVariable))
						{
							// Variable not yet in LocalVarToPinMap — this happens with
							// backward-jump bytecode patterns where the producer (e.g.,
							// GetAllActorsOfClass) is at a higher offset than the consumer
							// (e.g., Array_Length in a loop). Defer for post-processing.
							DeferredDataWires.Add(TPair<UEdGraphPin*, FName>(
								InputPin, ArgExpr->PropertyRef->GetFName()));
						}
						DataY += NodeSpacingY / 2;
					}
				}
			}

			ArgIndex++;
		}

		// Handle context object wiring (if this was wrapped in EX_Context)
		// Skip if the context object is EX_ObjectConst — these are world context objects
		// (e.g., UAkGameplayStatics::GetDefaultObject()) that the BP compiler auto-provides.
		// They should NOT be wired to the Target pin.
		if (Expr->ContextObject && Expr->ContextObject->Token != EX_ObjectConst)
		{
			UEdGraphPin* TargetPin = FindDataPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);

			// Library functions (e.g., UKismetArrayLibrary) have no Self pin.
			// The bytecode context maps to the first parameter (e.g., TargetArray).
			if (!TargetPin)
			{
				TargetPin = FindDataPin(CallNode, EGPD_Input, TEXT("TargetArray"));
			}
			if (!TargetPin)
			{
				// Fall back to first unconnected data input pin
				for (UEdGraphPin* Pin : CallNode->Pins)
				{
					if (Pin->Direction == EGPD_Input &&
						Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
						Pin->LinkedTo.Num() == 0)
					{
						TargetPin = Pin;
						break;
					}
				}
			}

			if (TargetPin && TargetPin->LinkedTo.Num() == 0)
			{
				UEdGraphPin* ContextOut = ResolveDataExpr(Graph, Expr->ContextObject, DataX, DataY);
				if (!ForceConnect(ContextOut, TargetPin))
				{
					Warn(FString::Printf(TEXT("Failed to wire context object to Target pin of %s"),
						*TargetFunc->GetName()));
				}
			}
		}

		// ── Array operation wildcard pin resolution (statement path) ───
		{
			UClass* FuncClass = TargetFunc->GetOwnerClass();
			if (FuncClass && FuncClass->GetName().Contains(TEXT("KismetArrayLibrary")))
			{
				FEdGraphPinType ElemType;
				bool bFoundElemType = false;

				if (Expr->ContextObject && Expr->ContextObject->PropertyRef)
				{
					FArrayProperty* ArrayProp = CastField<FArrayProperty>(Expr->ContextObject->PropertyRef);
					if (ArrayProp && ArrayProp->Inner)
					{
						GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(ArrayProp->Inner, ElemType);
						bFoundElemType = true;
					}
				}

				if (!bFoundElemType)
				{
					UEdGraphPin* ArrayPin = FindDataPin(CallNode, EGPD_Input, TEXT("TargetArray"));
					if (!ArrayPin || ArrayPin->LinkedTo.Num() == 0)
					{
						ArrayPin = FindDataPin(CallNode, EGPD_Input, TEXT("Target Array"));
					}
					if (!ArrayPin || ArrayPin->LinkedTo.Num() == 0)
					{
						for (UEdGraphPin* Pin : CallNode->Pins)
						{
							if (Pin->Direction == EGPD_Input &&
								Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
								Pin->LinkedTo.Num() > 0 &&
								(Pin->PinType.ContainerType == EPinContainerType::Array ||
									(Pin->LinkedTo[0]->PinType.ContainerType == EPinContainerType::Array)))
							{
								ArrayPin = Pin;
								break;
							}
						}
					}
					if (ArrayPin && ArrayPin->LinkedTo.Num() > 0)
					{
						FEdGraphPinType ConnectedType = ArrayPin->LinkedTo[0]->PinType;
						if (ConnectedType.ContainerType == EPinContainerType::Array)
						{
							ElemType = ConnectedType;
							ElemType.ContainerType = EPinContainerType::None;
							bFoundElemType = true;
						}
					}
				}

				if (bFoundElemType)
				{
					for (UEdGraphPin* Pin : CallNode->Pins)
					{
						if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
						{
							if (Pin->PinType.ContainerType == EPinContainerType::Array)
							{
								Pin->PinType = ElemType;
								Pin->PinType.ContainerType = EPinContainerType::Array;
							}
							else
							{
								Pin->PinType = ElemType;
							}
						}
					}
				}
			}
		}

		return CallNode;
	}

	// MULTICAST DELEGATE BROADCAST → UK2Node_CallDelegate
	// EX_CallMulticastDelegate broadcasts a multicast delegate.
	// Bytecode structure:
	//   FunctionRef  = signature function (e.g., OnSetupWalkSpeed__DelegateSignature)
	//   Children[0]  = delegate self-struct (EX_InstanceVariable or EX_Context)
	//   Children[1..N] = call arguments matching the signature
	case EX_CallMulticastDelegate:
	{
		UFunction* SigFunc = Expr->FunctionRef;
		if (!SigFunc)
		{
			Warn(FString::Printf(TEXT("Null signature function for CallMulticastDelegate at offset 0x%04X"), Expr->StartOffset));
			return nullptr;
		}

		if (Expr->Children.Num() < 1)
		{
			Warn(FString::Printf(TEXT("CallMulticastDelegate has no children (expected delegate ref) at offset 0x%04X"), Expr->StartOffset));
			return nullptr;
		}

		// Extract the multicast delegate property from Children[0] (delegate self-struct)
		TSharedPtr<FDecompiledExpr> DelegateExpr = Expr->Children[0];
		FProperty* DelegateProp = nullptr;
		bool bSelfContext = false;
		TSharedPtr<FDecompiledExpr> TargetObjectExpr;

		if (DelegateExpr)
		{
			if (DelegateExpr->Token == EX_InstanceVariable && DelegateExpr->PropertyRef)
			{
				// Self delegate — property is directly on this Blueprint
				DelegateProp = CastField<FProperty>(DelegateExpr->PropertyRef);
				bSelfContext = true;
			}
			else if (DelegateExpr->Token == EX_LocalVariable && DelegateExpr->PropertyRef)
			{
				// Local delegate variable (e.g., function-scope multicast delegate)
				DelegateProp = CastField<FProperty>(DelegateExpr->PropertyRef);
				bSelfContext = true;
			}
			else if (DelegateExpr->Token == EX_DefaultVariable && DelegateExpr->PropertyRef)
			{
				// Default (CDO) delegate property
				DelegateProp = CastField<FProperty>(DelegateExpr->PropertyRef);
				bSelfContext = true;
			}
			else if (DelegateExpr->Token == EX_Context || DelegateExpr->Token == EX_Context_FailSilent)
			{
				// Delegate on another object
				TargetObjectExpr = DelegateExpr->ContextObject;
				if (DelegateExpr->FieldRef)
				{
					DelegateProp = CastField<FProperty>(DelegateExpr->FieldRef);
				}
				if (!DelegateProp && DelegateExpr->Children.Num() > 0 &&
					DelegateExpr->Children[0] && DelegateExpr->Children[0]->PropertyRef)
				{
					DelegateProp = CastField<FProperty>(DelegateExpr->Children[0]->PropertyRef);
				}
			}
			// Generic fallback: if DelegateExpr has a PropertyRef, try it
			if (!DelegateProp && DelegateExpr->PropertyRef)
			{
				DelegateProp = CastField<FProperty>(DelegateExpr->PropertyRef);
				bSelfContext = true; // Assume self context for direct property refs
			}
		}

		// Fallback: derive delegate property from signature function name
		if (!DelegateProp && SigFunc)
		{
			FString SigName = SigFunc->GetName();
			if (SigName.RemoveFromEnd(TEXT("__DelegateSignature")))
			{
				UClass* SigOwner = SigFunc->GetOwnerClass();
				if (SigOwner)
				{
					// Search including super classes — delegate may be inherited
					for (TFieldIterator<FProperty> It(SigOwner); It; ++It)
					{
						if (It->GetFName() == FName(*SigName))
						{
							DelegateProp = *It;
							bSelfContext = (CurrentOriginalClass && CurrentOriginalClass->IsChildOf(SigOwner));
							break;
						}
					}
				}
				// Also try searching the original class hierarchy if SigOwner search failed
				if (!DelegateProp && CurrentOriginalClass)
				{
					for (TFieldIterator<FProperty> It(CurrentOriginalClass); It; ++It)
					{
						if (It->GetFName() == FName(*SigName))
						{
							DelegateProp = *It;
							bSelfContext = true;
							break;
						}
					}
				}
			}
		}

		if (!DelegateProp)
		{
			// Fallback: could not find delegate property — fall back to UK2Node_CallFunction.
			// This preserves the old behavior (compiler may warn but the node exists and
			// the exec chain stays intact, avoiding lost functionality).
			Warn(FString::Printf(TEXT("Could not extract delegate property for CallMulticastDelegate '%s' — falling back to CallFunction at offset 0x%04X"),
				*SigFunc->GetName(), Expr->StartOffset));

			// Temporarily redirect to the function call handler
			Expr->Token = EX_FinalFunction;
			UEdGraphNode* FallbackNode = CreateNodeForExpr(Graph, Expr, X, Y);
			Expr->Token = EX_CallMulticastDelegate; // Restore
			return FallbackNode;
		}

		UClass* OwnerClass = DelegateProp->GetOwnerClass();

		UK2Node_CallDelegate* CallDelegateNode = NewObject<UK2Node_CallDelegate>(Graph);
		CallDelegateNode->CreateNewGuid();
		CallDelegateNode->SetFromProperty(DelegateProp, bSelfContext, OwnerClass);
		CallDelegateNode->NodePosX = X;
		CallDelegateNode->NodePosY = Y;
		CallDelegateNode->AllocateDefaultPins();
		Graph->AddNode(CallDelegateNode, false, false);

		// Wire target object to Self pin (if not self context)
		if (!bSelfContext && TargetObjectExpr)
		{
			UEdGraphPin* SelfPin = FindDataPin(CallDelegateNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
			if (SelfPin)
			{
				int32 DataX = X - 200;
				int32 DataY = Y + DataNodeOffsetY;
				UEdGraphPin* TargetPin = ResolveDataExpr(Graph, TargetObjectExpr, DataX, DataY);
				ForceConnect(TargetPin, SelfPin);
			}
		}

		// Wire argument data pins (skip Children[0] which is the delegate self-struct)
		{
			int32 DataX = X - 200;
			int32 DataY = Y + DataNodeOffsetY;

			for (int32 ArgIndex = 1; ArgIndex < Expr->Children.Num(); ArgIndex++)
			{
				TSharedPtr<FDecompiledExpr> ArgExpr = Expr->Children[ArgIndex];
				if (!ArgExpr) continue;

				// Parameter index is (ArgIndex - 1) because Children[0] is the delegate ref
				FName ParamPinName = GetPinNameForFunctionParam(SigFunc, ArgIndex - 1);

				UEdGraphPin* InputPin = nullptr;
				if (ParamPinName != NAME_None)
				{
					InputPin = FindDataPin(CallDelegateNode, EGPD_Input, ParamPinName);
				}

				// Fallback: match by index among non-exec, non-self input pins
				if (!InputPin)
				{
					int32 DataPinIdx = 0;
					for (UEdGraphPin* Pin : CallDelegateNode->Pins)
					{
						if (Pin->Direction == EGPD_Input &&
							Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
							Pin->PinName != UEdGraphSchema_K2::PN_Self)
						{
							if (DataPinIdx == (ArgIndex - 1))
							{
								InputPin = Pin;
								break;
							}
							DataPinIdx++;
						}
					}
				}

				if (InputPin)
				{
					UEdGraphPin* DataPin = ResolveDataExpr(Graph, ArgExpr, DataX, DataY);
					if (DataPin)
					{
						ForceConnect(DataPin, InputPin);
					}
				}
				DataY += DataNodeOffsetY;
			}
		}

		X += NodeSpacingX;
		return CallDelegateNode;
	}

	// Virtual function calls (name-based)
	case EX_VirtualFunction:
	case EX_LocalVirtualFunction:
	{
		// Try to find the function by name
		// Search in the original class, its parents, and common libraries
		UFunction* ResolvedFunc = nullptr;
		FName FuncName = Expr->NameValue;

		// CRITICAL: If there's a context object (EX_Context wrapping), resolve
		// from the context type FIRST. Without this, functions that exist on both
		// the self class and the context class (e.g., ShowInstructions defined on
		// Gregory_C AND on PlayerHUD_C) incorrectly resolve to the self class,
		// losing the context wire and creating "Target must have a connection" errors.
		if (Expr->ContextObject)
		{
			// Walk the context chain to find class types
			TSharedPtr<FDecompiledExpr> CtxExpr = Expr->ContextObject;
			while (CtxExpr && !ResolvedFunc)
			{
				// First check: FieldRef (propagated from EX_Context R-value property).
				// This carries the specific type info even when the expression itself
				// is a generic local variable (e.g., ObjectProperty without class type).
				// Example: EX_Context wrapping GetPlayerHUD() has FieldRef = PlayerHUD_C
				// property, propagated to the local var expression.
				if (CtxExpr->FieldRef)
				{
					FProperty* Prop = CastField<FProperty>(CtxExpr->FieldRef);
					if (Prop)
					{
						FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
						if (ObjProp && ObjProp->PropertyClass)
						{
							ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
						}
						if (!ResolvedFunc)
						{
							FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(Prop);
							if (IntProp && IntProp->InterfaceClass)
							{
								ResolvedFunc = IntProp->InterfaceClass->FindFunctionByName(FuncName);
							}
						}
					}
				}
				// Check if context is an EX_InterfaceContext wrapping a variable
				if (!ResolvedFunc && CtxExpr->Token == EX_InterfaceContext && CtxExpr->Children.Num() > 0)
				{
					TSharedPtr<FDecompiledExpr> InnerExpr = CtxExpr->Children[0];
					if (InnerExpr && InnerExpr->PropertyRef)
					{
						FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(InnerExpr->PropertyRef);
						if (IntProp && IntProp->InterfaceClass)
						{
							ResolvedFunc = IntProp->InterfaceClass->FindFunctionByName(FuncName);
						}
						if (!ResolvedFunc)
						{
							FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(InnerExpr->PropertyRef);
							if (ObjProp && ObjProp->PropertyClass)
							{
								ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
							}
						}
					}
				}
				// Check variable's PropertyRef (direct type from FProperty)
				if (!ResolvedFunc && CtxExpr->PropertyRef)
				{
					FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(CtxExpr->PropertyRef);
					if (ObjProp && ObjProp->PropertyClass)
					{
						ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
					}
					if (!ResolvedFunc)
					{
						FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(CtxExpr->PropertyRef);
						if (IntProp && IntProp->InterfaceClass)
						{
							ResolvedFunc = IntProp->InterfaceClass->FindFunctionByName(FuncName);
						}
					}
				}
				// Check if context is a function call result — infer from return type
				if (!ResolvedFunc && CtxExpr->FunctionRef)
				{
					UFunction* CtxFunc = CtxExpr->FunctionRef;
					FProperty* RetProp = CtxFunc->GetReturnProperty();
					if (RetProp)
					{
						FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(RetProp);
						if (ObjProp && ObjProp->PropertyClass)
						{
							ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
						}
					}
				}
				// Check if context is an object constant (e.g., CDO Default__BFL_Post_C)
				// — the object's class owns the function we're looking for.
				if (!ResolvedFunc && CtxExpr->ObjectRef)
				{
					UClass* ObjClass = CtxExpr->ObjectRef->GetClass();
					if (ObjClass)
					{
						ResolvedFunc = ObjClass->FindFunctionByName(FuncName);
					}
				}

				// Walk deeper into the context chain
				if (CtxExpr->ContextObject)
				{
					CtxExpr = CtxExpr->ContextObject;
				}
				else if (CtxExpr->Children.Num() > 0)
				{
					CtxExpr = CtxExpr->Children[0];
				}
				else
				{
					break;
				}
			}
		}

		// Fall back to self-class resolution if no context or context didn't resolve
		if (!ResolvedFunc && CurrentOriginalClass)
		{
			ResolvedFunc = CurrentOriginalClass->FindFunctionByName(FuncName);
		}

		// Then search the Blueprint's parent class
		if (!ResolvedFunc && Graph->GetOuter())
		{
			UBlueprint* BP = Cast<UBlueprint>(Graph->GetOuter());
			if (BP && BP->ParentClass)
			{
				ResolvedFunc = BP->ParentClass->FindFunctionByName(FuncName);
			}
		}

		if (!ResolvedFunc)
		{
			// Search in common function libraries
			ResolvedFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FuncName);
		}
		if (!ResolvedFunc)
		{
			ResolvedFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(FuncName);
		}
		if (!ResolvedFunc)
		{
			// Search UGameplayStatics if available
			UClass* GameplayStatics = FindObject<UClass>(ANY_PACKAGE, TEXT("GameplayStatics"));
			if (GameplayStatics)
			{
				ResolvedFunc = GameplayStatics->FindFunctionByName(FuncName);
			}
		}

		if (ResolvedFunc)
		{
			// Reuse the same logic as resolved function calls
			Expr->FunctionRef = ResolvedFunc;
			Expr->Token = EX_FinalFunction; // Temporarily change to reuse logic
			bIsRedirectedVirtualCall = true; // Prevent parent call detection (virtual calls are never Super::)
			UEdGraphNode* Result = CreateNodeForExpr(Graph, Expr, X, Y);
			bIsRedirectedVirtualCall = false;
			Expr->Token = EX_VirtualFunction; // Restore
			return Result;
		}
		else
		{
			// Create a comment node indicating an unresolved virtual call
			Warn(FString::Printf(TEXT("Could not resolve virtual function: %s"), *FuncName.ToString()));

			// Create a placeholder using CallFunction with a note
			// For now, we'll just skip and log
			return nullptr;
		}
	}

	// BRANCH (JumpIfNot) → UK2Node_IfThenElse
	case EX_JumpIfNot:
	case EX_PopExecutionFlowIfNot:
	{
		UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
		BranchNode->CreateNewGuid();
		BranchNode->NodePosX = X;
		BranchNode->NodePosY = Y;
		BranchNode->AllocateDefaultPins();
		Graph->AddNode(BranchNode, false, false);

		// Wire the condition
		if (Expr->Children.Num() > 0 && Expr->Children[0])
		{
			UEdGraphPin* ConditionPin = FindDataPin(BranchNode, EGPD_Input, UEdGraphSchema_K2::PN_Condition);
			if (ConditionPin)
			{
				int32 CondX = X - 200;
				int32 CondY = Y + DataNodeOffsetY;

				if (Expr->Children[0]->IsLiteral())
				{
					ConditionPin->DefaultValue = Expr->Children[0]->GetLiteralAsString();
				}
				else
				{
					UEdGraphPin* CondOut = ResolveDataExpr(Graph, Expr->Children[0], CondX, CondY);
					TryConnect(CondOut, ConditionPin);
				}
			}
		}

		return BranchNode;
	}

	// ASSIGNMENTS (Let) → UK2Node_VariableSet
	case EX_Let:
	case EX_LetObj:
	case EX_LetBool:
	case EX_LetDelegate:
	case EX_LetMulticastDelegate:
	case EX_LetWeakObjPtr:
	{
		// Children[0] = destination (variable), Children[1] = source value
		if (Expr->Children.Num() < 2) return nullptr;

		TSharedPtr<FDecompiledExpr> DestExpr = Expr->Children[0];
		TSharedPtr<FDecompiledExpr> SourceExpr = Expr->Children[1];

		if (!DestExpr || !SourceExpr) return nullptr;

		// If destination is a local/instance variable, create a VariableSet node
		if (DestExpr->PropertyRef &&
			(DestExpr->Token == EX_LocalVariable || DestExpr->Token == EX_InstanceVariable ||
				DestExpr->Token == EX_DefaultVariable || DestExpr->Token == EX_LocalOutVariable))
		{
			// Check if this is an instance variable (blueprint-visible) or local
			if (DestExpr->Token == EX_InstanceVariable)
			{
				UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
				SetNode->CreateNewGuid();
				SetNode->VariableReference.SetSelfMember(DestExpr->PropertyRef->GetFName());
				SetNode->NodePosX = X;
				SetNode->NodePosY = Y;
				SetNode->AllocateDefaultPins();
				Graph->AddNode(SetNode, false, false);

				// Wire the source value to the input pin
				FName VarPinName = DestExpr->PropertyRef->GetFName();
				UEdGraphPin* ValuePin = FindDataPin(SetNode, EGPD_Input, VarPinName);
				if (!ValuePin)
				{
					// Try to find any input data pin
					for (UEdGraphPin* Pin : SetNode->Pins)
					{
						if (Pin->Direction == EGPD_Input &&
							Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							ValuePin = Pin;
							break;
						}
					}
				}

				if (ValuePin)
				{
					int32 DataX = X - 200;
					int32 DataY = Y + DataNodeOffsetY;
					if (SourceExpr->IsLiteral())
					{
						if (SourceExpr->Token == EX_NoObject)
						{
							// Null object — leave default empty
							ValuePin->DefaultValue = TEXT("");
						}
						else if (SourceExpr->Token == EX_ObjectConst && SourceExpr->ObjectRef)
						{
							ValuePin->DefaultObject = SourceExpr->ObjectRef;
						}
						else
						{
							// Enum-aware: resolve byte/int literals to enum names when pin expects an enum
							bool bEnumResolved = false;
							if ((SourceExpr->Token == EX_ByteConst || SourceExpr->Token == EX_IntConst ||
								SourceExpr->Token == EX_IntConstByte || SourceExpr->Token == EX_IntZero || SourceExpr->Token == EX_IntOne) &&
								ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
								ValuePin->PinType.PinSubCategoryObject.IsValid())
							{
								UEnum* PinEnum = Cast<UEnum>(ValuePin->PinType.PinSubCategoryObject.Get());
								if (PinEnum)
								{
									int64 Val = (SourceExpr->Token == EX_ByteConst) ? SourceExpr->ByteValue : SourceExpr->IntValue;
									FString EnumName = PinEnum->GetNameStringByValue(Val);
									if (!EnumName.IsEmpty())
									{
										ValuePin->DefaultValue = EnumName;
										bEnumResolved = true;
									}
								}
							}
							if (!bEnumResolved)
							{
								ValuePin->DefaultValue = SourceExpr->GetLiteralAsString();
							}
						}
					}
					else if (ApplyTempLiteralToPin(SourceExpr, ValuePin))
					{
						// Temp_ literal applied as pin default
					}
					else
					{
						UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
						ForceConnect(SourcePin, ValuePin);
					}
				}

				return SetNode;
			}
			else
			{
				// Local variable assignment — this is how the compiler stores intermediate
				// results (function return values, cast outputs, etc.) into temp variables.
				// Instead of creating VariableSet/VariableGet pairs, we record the output pin
				// so later reads of this variable wire directly to the source.
				FName VarName = DestExpr->PropertyRef->GetFName();

				// Multi-assigned Temp_ vars use TemporaryVariable nodes for reads.
				// Don't pollute LocalVarToPinMap with their write-side pins.
				const bool bIsMultiAssigned = MultiAssignedTempVars.Contains(VarName);

				// ── Multi-assigned variable: create AssignmentStatement ──
				// Loop counters, array indices, and accumulators need actual
				// assignment nodes to mutate the TemporaryVariable at runtime.
				// Without this, the TemporaryVariable always reads its initial
				// value (e.g., 0) and the variable never accumulates.
				if (bIsMultiAssigned)
				{
					UK2Node_AssignmentStatement* AssignNode = NewObject<UK2Node_AssignmentStatement>(Graph);
					AssignNode->CreateNewGuid();
					AssignNode->NodePosX = X;
					AssignNode->NodePosY = Y;
					AssignNode->AllocateDefaultPins();
					Graph->AddNode(AssignNode, false, false);

					// Wire Variable pin ← TemporaryVariable output
					UEdGraphPin* VarPin = AssignNode->GetVariablePin();
					if (VarPin)
					{
						// Get or create the TemporaryVariable for this multi-assigned var
						UEdGraphPin* TempVarOutPin = nullptr;
						if (UEdGraphPin** CachedPin = MultiAssignedTempPins.Find(VarName))
						{
							TempVarOutPin = *CachedPin;
						}
						else
						{
							// Create a new TemporaryVariable node
							UK2Node_TemporaryVariable* TempNode = NewObject<UK2Node_TemporaryVariable>(Graph);
							TempNode->CreateNewGuid();
							FEdGraphPinType VarType;
							GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(DestExpr->PropertyRef, VarType);
							TempNode->VariableType = VarType;
							TempNode->NodePosX = X - NodeSpacingX / 2;
							TempNode->NodePosY = Y - 150;
							TempNode->AllocateDefaultPins();
							Graph->AddNode(TempNode, false, false);
							TempVarOutPin = FindFirstDataOutputPin(TempNode);
							if (TempVarOutPin)
							{
								MultiAssignedTempPins.Add(VarName, TempVarOutPin);
							}
						}
						if (TempVarOutPin)
						{
							TryConnect(TempVarOutPin, VarPin);
							// Match the Value pin type to the Variable pin type
							UEdGraphPin* ValPin = AssignNode->GetValuePin();
							if (ValPin)
							{
								ValPin->PinType = TempVarOutPin->PinType;
							}
						}
					}

					// Wire Value pin ← source expression
					UEdGraphPin* ValPin = AssignNode->GetValuePin();
					UE_LOG(LogGraphBuilder, Log, TEXT("MultiAssign EX_Let: Var=%s SourceToken=0x%02X IsLiteral=%d ValPin=%s PinCat=%s SubCatValid=%d"),
						*VarName.ToString(), (uint8)SourceExpr->Token, SourceExpr->IsLiteral() ? 1 : 0,
						ValPin ? TEXT("valid") : TEXT("NULL"),
						ValPin ? *ValPin->PinType.PinCategory.ToString() : TEXT("N/A"),
						(ValPin && ValPin->PinType.PinSubCategoryObject.IsValid()) ? 1 : 0);
					if (ValPin)
					{
						int32 DataX = X - 200;
						int32 DataY = Y + DataNodeOffsetY;
						if (SourceExpr->IsLiteral())
						{
							if (SourceExpr->Token == EX_NoObject)
							{
								ValPin->DefaultValue = TEXT("");
							}
							else if (SourceExpr->Token == EX_ObjectConst && SourceExpr->ObjectRef)
							{
								ValPin->DefaultObject = SourceExpr->ObjectRef;
							}
							else
							{
								// Enum-aware: resolve byte/int literals to enum names when pin expects an enum
								bool bEnumResolved = false;
								if ((SourceExpr->Token == EX_ByteConst || SourceExpr->Token == EX_IntConst ||
									SourceExpr->Token == EX_IntConstByte || SourceExpr->Token == EX_IntZero || SourceExpr->Token == EX_IntOne) &&
									ValPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
									ValPin->PinType.PinSubCategoryObject.IsValid())
								{
									UEnum* PinEnum = Cast<UEnum>(ValPin->PinType.PinSubCategoryObject.Get());
									if (PinEnum)
									{
										int64 Val = (SourceExpr->Token == EX_ByteConst) ? SourceExpr->ByteValue : SourceExpr->IntValue;
										FString EnumName = PinEnum->GetNameStringByValue(Val);
										UE_LOG(LogGraphBuilder, Log, TEXT("  EnumResolve: Enum=%s Val=%lld Name='%s'"),
											*PinEnum->GetName(), Val, *EnumName);
										if (!EnumName.IsEmpty())
										{
											ValPin->DefaultValue = EnumName;
											bEnumResolved = true;
										}
									}
								}
								if (!bEnumResolved)
								{
									ValPin->DefaultValue = SourceExpr->GetLiteralAsString();
									UE_LOG(LogGraphBuilder, Log, TEXT("  Fallback literal: '%s'"), *ValPin->DefaultValue);
								}
							}
							UE_LOG(LogGraphBuilder, Log, TEXT("  After literal set: DefaultValue='%s'"), *ValPin->DefaultValue);
							// Save for re-application after RefreshAllNodes which can clear defaults
							DeferredPinDefaults.Add({AssignNode, ValPin->PinName, ValPin->DefaultValue, ValPin->DefaultObject});
						}
						else if (ApplyTempLiteralToPin(SourceExpr, ValPin))
						{
							// Temp_ literal applied as pin default
							UE_LOG(LogGraphBuilder, Log, TEXT("  ApplyTempLiteralToPin succeeded"));
							DeferredPinDefaults.Add({AssignNode, ValPin->PinName, ValPin->DefaultValue, ValPin->DefaultObject});
						}
						else
						{
							UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
							ForceConnect(SourcePin, ValPin);
							UE_LOG(LogGraphBuilder, Log, TEXT("  ResolveDataExpr path, SourcePin=%s"),
								SourcePin ? TEXT("valid") : TEXT("NULL"));
						}
					}

					X += NodeSpacingX;
					return AssignNode;
				}

				// ── EX_LocalOutVariable: function out-param ──
				// Store the source pin in LocalVarToPinMap. The post-processing
				// pass after FunctionResult creation will wire it up.
				if (DestExpr->Token == EX_LocalOutVariable)
				{
					// Literals (EX_ByteConst, EX_IntConst, etc.) don't produce nodes
					// or pins — ResolveDataExpr returns nullptr for them. Store the
					// literal expression so the FunctionResult wiring can apply it as
					// a pin default value (e.g., enum byte constants on return pins).
					if (SourceExpr->IsLiteral())
					{
						OutParamLiteralMap.Add(VarName, SourceExpr);
						UE_LOG(LogGraphBuilder, Log, TEXT("EX_LocalOutVariable '%s' ← literal (token=0x%02X), stored in OutParamLiteralMap"),
							*VarName.ToString(), (uint8)SourceExpr->Token);
						return nullptr;
					}

					int32 DataX = X - 200;
					int32 DataY = Y + DataNodeOffsetY;
					UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
					if (SourcePin)
					{
						LocalVarToPinMap.Add(VarName, SourcePin);
						PersistentFrameVarMap.Add(VarName, SourcePin);
					}

					// If the source expression created a node needing exec wiring, return it
					if (SourcePin && SourcePin->GetOwningNode())
					{
						UEdGraphNode* SourceNode = SourcePin->GetOwningNode();
						if (FindExecPin(SourceNode, EGPD_Input))
						{
							return SourceNode;
						}
					}
					return nullptr;
				}

				if (SourceExpr->IsFunctionCall())
				{
					UEdGraphNode* CallNode = CreateNodeForExpr(Graph, SourceExpr, X, Y);
					if (CallNode)
					{
						// Find the return value output pin and record it
						UEdGraphPin* ReturnPin = FindFirstDataOutputPin(CallNode);
						if (ReturnPin)
						{
							// ── Auto-cast insertion for base→derived type mismatches ──
							// Functions like BeginDeferredActorSpawnFromClass return AActor*,
							// GetGameInstanceSubsystem returns UGameInstanceSubsystem*, etc.
							// The local variable is typed to the specific derived class.
							// PinSubCategoryObject patches don't survive save/load because
							// UK2Node_CallFunction::AllocateDefaultPins recreates pins from
							// the UFunction signature. Insert a DynamicCast node instead —
							// cast nodes store TargetType and correctly reconstruct on reload.
							UClass* ReturnClass = Cast<UClass>(ReturnPin->PinType.PinSubCategoryObject.Get());
							UClass* VarClass = nullptr;
							if (DestExpr->PropertyRef)
							{
								if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(DestExpr->PropertyRef))
								{
									VarClass = ObjProp->PropertyClass;
								}
							}

							if (VarClass && ReturnClass &&
								VarClass != ReturnClass &&
								VarClass->IsChildOf(ReturnClass))
							{
								// Need downcast: insert PURE DynamicCast node.
								// Pure casts have no exec pins — they're just type converters.
								// The CallNode stays as the statement node for exec chain wiring.
								// Data flows: CallNode.ReturnValue → PureCast.Object → CastResult
								UK2Node_DynamicCast* AutoCast = NewObject<UK2Node_DynamicCast>(Graph);
								AutoCast->CreateNewGuid();
								AutoCast->TargetType = VarClass;
								AutoCast->SetPurity(true);
								AutoCast->NodePosX = X + 200;
								AutoCast->NodePosY = Y;
								Graph->AddNode(AutoCast, false, false);
								AutoCast->AllocateDefaultPins();

								// Wire data: ReturnValue → Cast.Object
								UEdGraphPin* CastObjPin = AutoCast->GetCastSourcePin();
								if (CastObjPin)
								{
									ReturnPin->MakeLinkTo(CastObjPin);
								}

								// Map local var to typed cast output
								UEdGraphPin* CastResultPin = AutoCast->GetCastResultPin();
								if (CastResultPin && !bIsMultiAssigned)
								{
									LocalVarToPinMap.Add(VarName, CastResultPin);
									PersistentFrameVarMap.Add(VarName, CastResultPin);
								}

								// Don't return AutoCast — return CallNode below so it
								// stays in the exec chain (impure calls need exec flow)
							}
							else if (!bIsMultiAssigned)
							{
								LocalVarToPinMap.Add(VarName, ReturnPin);
								PersistentFrameVarMap.Add(VarName, ReturnPin);
							}
						}
					}
					return CallNode;
				}
				else if (SourceExpr->Token == EX_DynamicCast ||
					SourceExpr->Token == EX_ObjToInterfaceCast ||
					SourceExpr->Token == EX_CrossInterfaceCast ||
					SourceExpr->Token == EX_InterfaceToObjCast)
				{
					// Cast result — resolve it and record the output pin
					int32 DataX = X - 200;
					int32 DataY = Y;
					UEdGraphPin* CastOutPin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
					if (CastOutPin)
					{
						if (!bIsMultiAssigned)
						{
							LocalVarToPinMap.Add(VarName, CastOutPin);
							PersistentFrameVarMap.Add(VarName, CastOutPin);
						}

						UEdGraphNode* CastNode = CastOutPin->GetOwningNode();
						if (CastNode)
						{
							LastCastNode = CastNode;
							for (UEdGraphPin* Pin : CastNode->Pins)
							{
								if (Pin->Direction == EGPD_Output &&
									Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
								{
									LocalVarToPinMap.Add(TEXT("K2Node_DynamicCast_bSuccess"), Pin);
									break;
								}
							}
						}
					}
					if (CastOutPin && CastOutPin->GetOwningNode())
					{
						return CastOutPin->GetOwningNode();
					}
					return nullptr;
				}
				else
				{
					// Check if this is a Context-wrapped function call (e.g. obj.FuncCall())
					bool bIsContextCall = false;
					if (SourceExpr->Token == EX_Context || SourceExpr->Token == EX_Context_FailSilent)
					{
						if (SourceExpr->Children.Num() > 0 && SourceExpr->Children[0] &&
							SourceExpr->Children[0]->IsFunctionCall())
						{
							bIsContextCall = true;
						}
					}

					if (bIsContextCall)
					{
						// Context-wrapped call — create via CreateNodeForExpr to get exec wiring
						UEdGraphNode* CallNode = CreateNodeForExpr(Graph, SourceExpr, X, Y);
						if (CallNode && !bIsMultiAssigned)
						{
							UEdGraphPin* ReturnPin = FindFirstDataOutputPin(CallNode);
							if (ReturnPin)
							{
								LocalVarToPinMap.Add(VarName, ReturnPin);
								PersistentFrameVarMap.Add(VarName, ReturnPin);
							}
						}
						return CallNode;
					}

					// If this is a Temp_ variable being assigned a literal, store the
					// literal expression so it can be applied as a pin default later.
					// Don't try to resolve it — literals don't create graph nodes.
					// EXCEPTION: Multi-assigned variables (e.g. loop counters) must NOT
					// be optimized — their value changes at runtime.
					if (SourceExpr->IsLiteral() && VarName.ToString().StartsWith(TEXT("Temp_")) &&
						!bIsMultiAssigned)
					{
						TempLiteralMap.Add(VarName, SourceExpr);
						return nullptr;
					}

					// Generic expression — resolve to a pin and record it
					int32 DataX = X - 200;
					int32 DataY = Y;
					UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
					if (SourcePin && !bIsMultiAssigned)
					{
						LocalVarToPinMap.Add(VarName, SourcePin);
						PersistentFrameVarMap.Add(VarName, SourcePin);
					}
					// If the source created a node with exec pins, return it for exec wiring
					if (SourcePin && SourcePin->GetOwningNode())
					{
						UEdGraphNode* SourceNode = SourcePin->GetOwningNode();
						if (FindExecPin(SourceNode, EGPD_Input))
						{
							return SourceNode;
						}
					}
					return nullptr;
				}
			}
		}

		// ── MakeStruct inline pattern: StructVar.Member = Value ──
		// The compiler inlines MakeStruct as: EX_Let(EX_StructMemberContext[Member](LocalVar), Source)
		// We accumulate the member values into StructMemberAssignMap. When the struct variable
		// is later consumed (e.g., passed as argument to Array_Set), ResolveDataExpr creates
		// a UK2Node_MakeStruct with those values applied as pin defaults.
		if (DestExpr->Token == EX_StructMemberContext)
		{
			FProperty* MemberProp = DestExpr->PropertyRef;
			if (MemberProp && DestExpr->Children.Num() > 0 && DestExpr->Children[0])
			{
				TSharedPtr<FDecompiledExpr> StructVarExpr = DestExpr->Children[0];
				if (StructVarExpr->PropertyRef)
				{
					FName StructVarName = StructVarExpr->PropertyRef->GetFName();
					FName MemberName = MemberProp->GetFName();

					FStructMemberWrite& Entry = StructMemberAssignMap.FindOrAdd(StructVarName);
					if (!Entry.StructType)
					{
						Entry.StructType = Cast<UScriptStruct>(MemberProp->GetOwnerStruct());
					}
					Entry.MemberValues.Add(MemberName, SourceExpr);

					UE_LOG(LogGraphBuilder, Log, TEXT("MakeStruct accumulate: %s.%s (struct=%s)"),
						*StructVarName.ToString(), *MemberName.ToString(),
						Entry.StructType ? *Entry.StructType->GetName() : TEXT("unknown"));
					return nullptr; // No exec node — accumulate only
				}
			}
		}

		// ── Context-based property set: OtherObject.Property = Value ──
		// Handles patterns like: EX_Let(EX_Context(LocalVar, InstanceVariable[Prop]), Source)
		// Creates a VariableSet node with a Target pin wired to the context object.
		// Common in loop bodies: e.g., ConduitBar.PowerUpRate = SwitchValue(...)
		if (DestExpr->Token == EX_Context)
		{
			// Walk the context chain to find the target property
			FProperty* TargetProp = nullptr;
			TSharedPtr<FDecompiledExpr> ContextObjectExpr;

			TSharedPtr<FDecompiledExpr> WalkExpr = DestExpr;
			while (WalkExpr)
			{
				if (WalkExpr->Token == EX_Context)
				{
					// The context object is the first child or ContextObject field
					if (WalkExpr->ContextObject)
					{
						ContextObjectExpr = WalkExpr->ContextObject;
					}
					else if (WalkExpr->Children.Num() > 0)
					{
						ContextObjectExpr = WalkExpr->Children[0];
					}

					// The property being accessed may be deeper
					if (WalkExpr->Children.Num() > 0 && WalkExpr->Children.Last())
					{
						WalkExpr = WalkExpr->Children.Last();
						continue;
					}
					else if (WalkExpr->PropertyRef)
					{
						TargetProp = WalkExpr->PropertyRef;
						break;
					}
				}
				else if (WalkExpr->Token == EX_InstanceVariable && WalkExpr->PropertyRef)
				{
					TargetProp = WalkExpr->PropertyRef;
					break;
				}
				else if (WalkExpr->PropertyRef)
				{
					TargetProp = WalkExpr->PropertyRef;
					break;
				}
				break;
			}

			if (TargetProp)
			{
				FName VarName = TargetProp->GetFName();

				UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
				SetNode->CreateNewGuid();

				// Use SetExternalMember with the property's owning class (e.g., ConduitBar_C)
				// instead of SetSelfMember (which would look on the current BP's class).
				UClass* OwnerClass = TargetProp->GetOwnerClass();
				if (OwnerClass)
				{
					SetNode->VariableReference.SetExternalMember(VarName, OwnerClass);
				}
				else
				{
					SetNode->VariableReference.SetSelfMember(VarName);
				}
				SetNode->NodePosX = X;
				SetNode->NodePosY = Y;
				SetNode->AllocateDefaultPins();
				Graph->AddNode(SetNode, false, false);

				// Wire the context object to the Target pin
				if (ContextObjectExpr)
				{
					UEdGraphPin* TargetPin = FindDataPin(SetNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
					if (TargetPin)
					{
						int32 CtxX = X - 300;
						int32 CtxY = Y + DataNodeOffsetY;
						UEdGraphPin* CtxPin = ResolveDataExpr(Graph, ContextObjectExpr, CtxX, CtxY);
						if (CtxPin)
						{
							ForceConnect(CtxPin, TargetPin);
						}
					}
				}

				// Wire the source value to the variable input pin
				FName VarPinName = TargetProp->GetFName();
				UEdGraphPin* ValuePin = FindDataPin(SetNode, EGPD_Input, VarPinName);
				if (!ValuePin)
				{
					for (UEdGraphPin* Pin : SetNode->Pins)
					{
						if (Pin->Direction == EGPD_Input &&
							Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
							Pin->PinName != UEdGraphSchema_K2::PN_Self)
						{
							ValuePin = Pin;
							break;
						}
					}
				}

				if (ValuePin)
				{
					int32 DataX = X - 200;
					int32 DataY = Y + DataNodeOffsetY;
					if (SourceExpr->IsLiteral())
					{
						if (SourceExpr->Token == EX_NoObject)
						{
							ValuePin->DefaultValue = TEXT("");
						}
						else if (SourceExpr->Token == EX_ObjectConst && SourceExpr->ObjectRef)
						{
							ValuePin->DefaultObject = SourceExpr->ObjectRef;
						}
						else
						{
							bool bEnumResolved = false;
							if ((SourceExpr->Token == EX_ByteConst || SourceExpr->Token == EX_IntConst ||
								SourceExpr->Token == EX_IntConstByte || SourceExpr->Token == EX_IntZero || SourceExpr->Token == EX_IntOne) &&
								ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
								ValuePin->PinType.PinSubCategoryObject.IsValid())
							{
								UEnum* PinEnum = Cast<UEnum>(ValuePin->PinType.PinSubCategoryObject.Get());
								if (PinEnum)
								{
									int64 Val = (SourceExpr->Token == EX_ByteConst) ? SourceExpr->ByteValue : SourceExpr->IntValue;
									FString EnumName = PinEnum->GetNameStringByValue(Val);
									if (!EnumName.IsEmpty())
									{
										ValuePin->DefaultValue = EnumName;
										bEnumResolved = true;
									}
								}
							}
							if (!bEnumResolved)
							{
								ValuePin->DefaultValue = SourceExpr->GetLiteralAsString();
							}
						}
					}
					else if (ApplyTempLiteralToPin(SourceExpr, ValuePin))
					{
						// Temp_ literal applied
					}
					else
					{
						UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
						ForceConnect(SourcePin, ValuePin);
					}
				}

				UE_LOG(LogGraphBuilder, Log, TEXT("Context-based property set: %s (via context)"), *VarName.ToString());
				X += NodeSpacingX;
				return SetNode;
			}
		}

		// If the destination is something else (array element, struct member, etc.)
		// For Phase 1, try to emit the source expression if it's a function call
		if (SourceExpr->IsFunctionCall())
		{
			return CreateNodeForExpr(Graph, SourceExpr, X, Y);
		}

		return nullptr;
	}

	// LET ON PERSISTENT FRAME (ubergraph local variable assignment)
	case EX_LetValueOnPersistentFrame:
	{
		// PropertyRef = destination, Children[0] = source
		if (!Expr->PropertyRef || Expr->Children.Num() < 1) return nullptr;

		FName VarName = Expr->PropertyRef->GetFName();
		TSharedPtr<FDecompiledExpr> SourceExpr = Expr->Children[0];
		if (!SourceExpr) return nullptr;

		const bool bIsMultiAssignedPF = MultiAssignedTempVars.Contains(VarName);

		// ── Multi-assigned variable: create AssignmentStatement ──
		if (bIsMultiAssignedPF)
		{
			UK2Node_AssignmentStatement* AssignNode = NewObject<UK2Node_AssignmentStatement>(Graph);
			AssignNode->CreateNewGuid();
			AssignNode->NodePosX = X;
			AssignNode->NodePosY = Y;
			AssignNode->AllocateDefaultPins();
			Graph->AddNode(AssignNode, false, false);

			// Wire Variable pin ← TemporaryVariable output
			UEdGraphPin* VarPin = AssignNode->GetVariablePin();
			if (VarPin)
			{
				UEdGraphPin* TempVarOutPin = nullptr;
				if (UEdGraphPin** CachedPin = MultiAssignedTempPins.Find(VarName))
				{
					TempVarOutPin = *CachedPin;
				}
				else
				{
					UK2Node_TemporaryVariable* TempNode = NewObject<UK2Node_TemporaryVariable>(Graph);
					TempNode->CreateNewGuid();
					FEdGraphPinType VarType;
					GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Expr->PropertyRef, VarType);
					TempNode->VariableType = VarType;
					TempNode->NodePosX = X - NodeSpacingX / 2;
					TempNode->NodePosY = Y - 150;
					TempNode->AllocateDefaultPins();
					Graph->AddNode(TempNode, false, false);
					TempVarOutPin = FindFirstDataOutputPin(TempNode);
					if (TempVarOutPin)
					{
						MultiAssignedTempPins.Add(VarName, TempVarOutPin);
					}
				}
				if (TempVarOutPin)
				{
					TryConnect(TempVarOutPin, VarPin);
					UEdGraphPin* ValPin = AssignNode->GetValuePin();
					if (ValPin)
					{
						ValPin->PinType = TempVarOutPin->PinType;
					}
				}
			}

			// Wire Value pin ← source expression
			UEdGraphPin* ValPin = AssignNode->GetValuePin();
			UE_LOG(LogGraphBuilder, Log, TEXT("MultiAssign PersistFrame: Var=%s SourceToken=0x%02X IsLiteral=%d ValPin=%s PinCat=%s SubCatValid=%d"),
				*VarName.ToString(), (uint8)SourceExpr->Token, SourceExpr->IsLiteral() ? 1 : 0,
				ValPin ? TEXT("valid") : TEXT("NULL"),
				ValPin ? *ValPin->PinType.PinCategory.ToString() : TEXT("N/A"),
				(ValPin && ValPin->PinType.PinSubCategoryObject.IsValid()) ? 1 : 0);
			if (ValPin)
			{
				int32 DataX = X - 200;
				int32 DataY = Y + DataNodeOffsetY;
				if (SourceExpr->IsLiteral())
				{
					if (SourceExpr->Token == EX_NoObject)
					{
						ValPin->DefaultValue = TEXT("");
					}
					else if (SourceExpr->Token == EX_ObjectConst && SourceExpr->ObjectRef)
					{
						ValPin->DefaultObject = SourceExpr->ObjectRef;
					}
					else
					{
						// Enum-aware: resolve byte/int literals to enum names when pin expects an enum
						bool bEnumResolved = false;
						if ((SourceExpr->Token == EX_ByteConst || SourceExpr->Token == EX_IntConst ||
							SourceExpr->Token == EX_IntConstByte || SourceExpr->Token == EX_IntZero || SourceExpr->Token == EX_IntOne) &&
							ValPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
							ValPin->PinType.PinSubCategoryObject.IsValid())
						{
							UEnum* PinEnum = Cast<UEnum>(ValPin->PinType.PinSubCategoryObject.Get());
							if (PinEnum)
							{
								int64 Val = (SourceExpr->Token == EX_ByteConst) ? SourceExpr->ByteValue : SourceExpr->IntValue;
								FString EnumName = PinEnum->GetNameStringByValue(Val);
								UE_LOG(LogGraphBuilder, Log, TEXT("  PF EnumResolve: Enum=%s Val=%lld Name='%s'"),
									*PinEnum->GetName(), Val, *EnumName);
								if (!EnumName.IsEmpty())
								{
									ValPin->DefaultValue = EnumName;
									bEnumResolved = true;
								}
							}
						}
						if (!bEnumResolved)
						{
							ValPin->DefaultValue = SourceExpr->GetLiteralAsString();
							UE_LOG(LogGraphBuilder, Log, TEXT("  PF Fallback literal: '%s'"), *ValPin->DefaultValue);
						}
					}
					UE_LOG(LogGraphBuilder, Log, TEXT("  PF After literal set: DefaultValue='%s'"), *ValPin->DefaultValue);
					// Save for re-application after RefreshAllNodes which can clear defaults
					DeferredPinDefaults.Add({AssignNode, ValPin->PinName, ValPin->DefaultValue, ValPin->DefaultObject});
				}
				else if (ApplyTempLiteralToPin(SourceExpr, ValPin))
				{
					// Temp_ literal applied as pin default
					UE_LOG(LogGraphBuilder, Log, TEXT("  PF ApplyTempLiteralToPin succeeded"));
					DeferredPinDefaults.Add({AssignNode, ValPin->PinName, ValPin->DefaultValue, ValPin->DefaultObject});
				}
				else
				{
					UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
					ForceConnect(SourcePin, ValPin);
					UE_LOG(LogGraphBuilder, Log, TEXT("  PF ResolveDataExpr path, SourcePin=%s"),
						SourcePin ? TEXT("valid") : TEXT("NULL"));
				}
			}

			X += NodeSpacingX;
			return AssignNode;
		}

		if (SourceExpr->IsFunctionCall() ||
			(SourceExpr->Token == EX_Context || SourceExpr->Token == EX_Context_FailSilent))
		{
			UEdGraphNode* CallNode = CreateNodeForExpr(Graph, SourceExpr, X, Y);
			if (CallNode && !bIsMultiAssignedPF)
			{
				UEdGraphPin* ReturnPin = FindFirstDataOutputPin(CallNode);
				if (ReturnPin)
				{
					LocalVarToPinMap.Add(VarName, ReturnPin);
					PersistentFrameVarMap.Add(VarName, ReturnPin);
				}
			}
			return CallNode;
		}
		else if (SourceExpr->Token == EX_DynamicCast ||
			SourceExpr->Token == EX_ObjToInterfaceCast)
		{
			int32 DataX = X - 200;
			int32 DataY = Y;
			UEdGraphPin* CastOutPin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
			if (CastOutPin)
			{
				if (!bIsMultiAssignedPF)
				{
					LocalVarToPinMap.Add(VarName, CastOutPin);
					PersistentFrameVarMap.Add(VarName, CastOutPin);
				}

				// Also auto-map the bSuccess pin from this cast node
				UEdGraphNode* CastNode = CastOutPin->GetOwningNode();
				if (CastNode)
				{
					LastCastNode = CastNode;
					for (UEdGraphPin* Pin : CastNode->Pins)
					{
						if (Pin->Direction == EGPD_Output &&
							Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
						{
							// Store under the common bSuccess name pattern
							LocalVarToPinMap.Add(TEXT("K2Node_DynamicCast_bSuccess"), Pin);
							break;
						}
					}
				}

				if (CastNode)
					return CastNode;
			}
			return nullptr;
		}
		else
		{
			// If this is a Temp_ variable being assigned a literal, store it
			// (skip multi-assigned variables like loop counters)
			if (SourceExpr->IsLiteral() && VarName.ToString().StartsWith(TEXT("Temp_")) &&
				!bIsMultiAssignedPF)
			{
				TempLiteralMap.Add(VarName, SourceExpr);
				return nullptr;
			}

			int32 DataX = X - 200;
			int32 DataY = Y;
			UEdGraphPin* SourcePin = ResolveDataExpr(Graph, SourceExpr, DataX, DataY);
			if (SourcePin && !bIsMultiAssignedPF)
			{
				LocalVarToPinMap.Add(VarName, SourcePin);
				PersistentFrameVarMap.Add(VarName, SourcePin);
			}
			return nullptr;
		}
	}

	// SET ARRAY → K2Node_MakeArray (for array literal assignments)
	case EX_SetArray:
	{
		// Children[0] = target array variable, Children[1..N] = elements
		if (Expr->Children.Num() < 1) return nullptr;

		TSharedPtr<FDecompiledExpr> TargetExpr = Expr->Children[0];
		if (!TargetExpr || !TargetExpr->PropertyRef) return nullptr;

		FName ArrayVarName = TargetExpr->PropertyRef->GetFName();
		int32 NumElements = Expr->Children.Num() - 1;

		// Skip creating MakeArray for empty output-parameter arrays.
		// Pattern: EX_SetArray(CallFunc_Foo_OutArray, []) followed by EX_CallFunction(Foo).
		// The function call's output-parameter handler will map this variable to the correct
		// output pin. Creating an orphaned WILDCARD MakeArray node interferes with
		// RefreshAllNodes type propagation for CallArrayFunction nodes.
		if (NumElements == 0 && ArrayVarName.ToString().StartsWith(TEXT("CallFunc_")))
		{
			return nullptr;
		}

		// Create MakeArray node
		UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Graph);
		MakeArrayNode->CreateNewGuid();
		MakeArrayNode->NumInputs = FMath::Max(NumElements, 1);
		MakeArrayNode->NodePosX = X;
		MakeArrayNode->NodePosY = Y;
		MakeArrayNode->AllocateDefaultPins();
		Graph->AddNode(MakeArrayNode, false, false);

		// Determine element type from the array property
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(TargetExpr->PropertyRef);
		if (ArrayProp && ArrayProp->Inner)
		{
			FEdGraphPinType ElemPinType;
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(ArrayProp->Inner, ElemPinType);

			// Set output pin type
			for (UEdGraphPin* Pin : MakeArrayNode->Pins)
			{
				if (Pin->Direction == EGPD_Output)
				{
					Pin->PinType = ElemPinType;
					Pin->PinType.ContainerType = EPinContainerType::Array;
					break;
				}
			}

			// Set input pin types
			for (UEdGraphPin* Pin : MakeArrayNode->Pins)
			{
				if (Pin->Direction == EGPD_Input &&
					Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					Pin->PinType = ElemPinType;
				}
			}
		}

		// Wire element values
		int32 ElemX = X - 250;
		int32 ElemY = Y;
		for (int32 i = 0; i < NumElements; i++)
		{
			TSharedPtr<FDecompiledExpr> ElemExpr = Expr->Children[i + 1];
			if (!ElemExpr) continue;

			// Find the [i] input pin
			FName ElemPinName = *FString::Printf(TEXT("[%d]"), i);
			UEdGraphPin* ElemPin = FindDataPin(MakeArrayNode, EGPD_Input, ElemPinName);
			if (!ElemPin)
			{
				// Try numeric name
				ElemPinName = *FString::FromInt(i);
				ElemPin = FindDataPin(MakeArrayNode, EGPD_Input, ElemPinName);
			}
			if (!ElemPin)
			{
				// Try finding by index among input data pins
				int32 DataIdx = 0;
				for (UEdGraphPin* Pin : MakeArrayNode->Pins)
				{
					if (Pin->Direction == EGPD_Input &&
						Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						if (DataIdx == i) { ElemPin = Pin; break; }
						DataIdx++;
					}
				}
			}

			if (ElemPin)
			{
				// Handle EX_Self and EX_NoObject specially — they're "literals" but
				// need node wiring (Self) or empty default (NoObject), not string values
				if (ElemExpr->Token == EX_Self)
				{
					UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
					SelfNode->CreateNewGuid();
					SelfNode->NodePosX = ElemX;
					SelfNode->NodePosY = ElemY;
					SelfNode->AllocateDefaultPins();
					Graph->AddNode(SelfNode, false, false);
					UEdGraphPin* SelfOut = FindFirstDataOutputPin(SelfNode);
					ForceConnect(SelfOut, ElemPin);
					ElemY += NodeSpacingY / 3;
				}
				else if (ElemExpr->Token == EX_NoObject)
				{
					ElemPin->DefaultValue = TEXT("");
				}
				else if (ElemExpr->IsLiteral())
				{
					// Check if pin expects an enum — resolve name instead of raw int
					if ((ElemExpr->Token == EX_ByteConst || ElemExpr->Token == EX_IntConst ||
						ElemExpr->Token == EX_IntConstByte) &&
						ElemPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
						ElemPin->PinType.PinSubCategoryObject.IsValid())
					{
						UEnum* PinEnum = Cast<UEnum>(ElemPin->PinType.PinSubCategoryObject.Get());
						if (PinEnum)
						{
							int64 Val = (ElemExpr->Token == EX_ByteConst) ? ElemExpr->ByteValue : ElemExpr->IntValue;
							FString EnumName = PinEnum->GetNameStringByValue(Val);
							ElemPin->DefaultValue = EnumName.IsEmpty() ? ElemExpr->GetLiteralAsString() : EnumName;
						}
						else
						{
							ElemPin->DefaultValue = ElemExpr->GetLiteralAsString();
						}
					}
					else if (ElemExpr->Token == EX_ObjectConst)
					{
						if (ElemExpr->ObjectRef)
						{
							ElemPin->DefaultObject = ElemExpr->ObjectRef;
						}
					}
					else
					{
						ElemPin->DefaultValue = ElemExpr->GetLiteralAsString();
					}
				}
				else
				{
					UEdGraphPin* ElemOut = ResolveDataExpr(Graph, ElemExpr, ElemX, ElemY);
					ForceConnect(ElemOut, ElemPin);
					ElemY += NodeSpacingY / 3;
				}
			}
		}

		// Map the array output to the variable name so downstream reads resolve
		UEdGraphPin* ArrayOutPin = FindFirstDataOutputPin(MakeArrayNode);
		if (ArrayOutPin)
		{
			LocalVarToPinMap.Add(ArrayVarName, ArrayOutPin);
		}

		// MakeArray is a pure node — no exec pins to chain
		return nullptr;
	}

	// CONTEXT (function call on another object)
	case EX_Context:
	case EX_Context_FailSilent:
	{
		// The actual operation is in Children[0], with ContextObject as the target
		if (Expr->Children.Num() > 0 && Expr->Children[0])
		{
			TSharedPtr<FDecompiledExpr> InnerExpr = Expr->Children[0];

			// Chain context objects for nested access patterns like:
			//   Pawn.AttachmentComponent.GetWorldTransform()
			// Safety: avoid creating cycles when expression nodes are shared.
			// If InnerExpr->ContextObject IS Expr->ContextObject (same pointer),
			// or if Expr->ContextObject already appears in the chain, skip chaining.
			if (InnerExpr->ContextObject && Expr->ContextObject
				&& InnerExpr->ContextObject.Get() != Expr->ContextObject.Get())
			{
				// Walk the chain to make sure we won't create a cycle
				bool bWouldCycle = false;
				for (TSharedPtr<FDecompiledExpr> Walk = Expr->ContextObject; Walk; Walk = Walk->ContextObject)
				{
					if (Walk.Get() == InnerExpr->ContextObject.Get())
					{
						bWouldCycle = true;
						break;
					}
				}
				if (!bWouldCycle)
				{
					InnerExpr->ContextObject->ContextObject = Expr->ContextObject;
				}
			}
			else if (!InnerExpr->ContextObject)
			{
				InnerExpr->ContextObject = Expr->ContextObject;
			}

			// Propagate EX_Context's FieldRef to the ContextObject expression.
			// The FieldRef carries the R-value property type from the bytecode
			// (e.g., for GetPlayerHUD() result, FieldRef has PlayerHUD_C type).
			// This is critical for virtual function resolution on the context —
			// without it, a generic ObjectProperty local var won't carry the
			// specific class type needed to find functions like ShowInstructions.
			if (InnerExpr->ContextObject && Expr->FieldRef)
			{
				if (!InnerExpr->ContextObject->FieldRef)
				{
					InnerExpr->ContextObject->FieldRef = Expr->FieldRef;
				}
			}

			return CreateNodeForExpr(Graph, InnerExpr, X, Y);
		}
		return nullptr;
	}

	// CAST → UK2Node_DynamicCast
	case EX_DynamicCast:
	{
		if (!Expr->ClassRef) return nullptr;

		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
		CastNode->CreateNewGuid();
		CastNode->TargetType = Expr->ClassRef;
		CastNode->NodePosX = X;
		CastNode->NodePosY = Y;
		CastNode->AllocateDefaultPins();
		Graph->AddNode(CastNode, false, false);

		// Widen Object pin to UObject so ForceConnect doesn't trigger
		// "already is X" or "always fail" warnings from type narrowing
		{
			UEdGraphPin* ObjPin = FindDataPin(CastNode, EGPD_Input, TEXT("Object"));
			if (!ObjPin)
			{
				for (UEdGraphPin* Pin : CastNode->Pins)
				{
					if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						ObjPin = Pin;
						break;
					}
				}
			}
			if (ObjPin)
			{
				ObjPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				ObjPin->PinType.PinSubCategoryObject = UObject::StaticClass();
			}
		}

		// Wire the input object
		if (Expr->Children.Num() > 0 && Expr->Children[0])
		{
			UEdGraphPin* ObjectPin = FindDataPin(CastNode, EGPD_Input, TEXT("Object"));
			if (!ObjectPin)
			{
				// Try to find the first input data pin
				for (UEdGraphPin* Pin : CastNode->Pins)
				{
					if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						ObjectPin = Pin;
						break;
					}
				}
			}

			if (ObjectPin)
			{
				int32 DataX = X - 200;
				int32 DataY = Y + DataNodeOffsetY;
				UEdGraphPin* InputPin = ResolveDataExpr(Graph, Expr->Children[0], DataX, DataY);
				// Use ForceConnect — cast inputs may come from context chains with unresolved types
				ForceConnect(InputPin, ObjectPin);
			}
		}

		return CastNode;
	}

	// DELEGATE OPERATIONS

	// EX_BindDelegate → UK2Node_CreateDelegate (pure node)
	// Binds a function name to a delegate variable for later use by Add/RemoveMulticastDelegate.
	//   NameValue    = function name to bind (e.g., "On Item Removed")
	//   Children[0]  = delegate output variable (local var that receives the bound delegate)
	//   Children[1]  = target object (Self or another object whose function to bind)
	case EX_BindDelegate:
	{
		if (Expr->Children.Num() < 2) return nullptr;

		UK2Node_CreateDelegate* CreateDelegateNode = NewObject<UK2Node_CreateDelegate>(Graph);
		CreateDelegateNode->CreateNewGuid();
		CreateDelegateNode->SelectedFunctionName = Expr->NameValue;
		CreateDelegateNode->NodePosX = X - 200;
		CreateDelegateNode->NodePosY = Y + DataNodeOffsetY;
		CreateDelegateNode->AllocateDefaultPins();
		Graph->AddNode(CreateDelegateNode, false, false);

		// Wire target object to Self pin (skip if Self — default is correct)
		if (Expr->Children[1] && Expr->Children[1]->Token != EX_Self)
		{
			UEdGraphPin* SelfPin = CreateDelegateNode->GetObjectInPin();
			if (SelfPin)
			{
				int32 DataX = X - 400;
				int32 DataY = Y + DataNodeOffsetY;
				UEdGraphPin* TargetPin = ResolveDataExpr(Graph, Expr->Children[1], DataX, DataY);
				ForceConnect(TargetPin, SelfPin);
			}
		}

		// Map the delegate output variable to the OutputDelegate pin
		if (Expr->Children[0] && Expr->Children[0]->PropertyRef)
		{
			FName VarName = Expr->Children[0]->PropertyRef->GetFName();
			UEdGraphPin* OutputDelegatePin = CreateDelegateNode->GetDelegateOutPin();
			if (OutputDelegatePin)
			{
				LocalVarToPinMap.Add(VarName, OutputDelegatePin);
			}
		}

		// Pure node — no exec wiring needed
		return nullptr;
	}

	// EX_AddMulticastDelegate → UK2Node_AddDelegate
	// EX_RemoveMulticastDelegate → UK2Node_RemoveDelegate
	// Adds/removes a delegate binding to a multicast delegate property.
	//   Children[0] = multicast delegate expression (property reference, possibly via EX_Context)
	//   Children[1] = delegate to add/remove (local variable from EX_BindDelegate)
	case EX_AddMulticastDelegate:
	case EX_RemoveMulticastDelegate:
	{
		if (Expr->Children.Num() < 2) return nullptr;

		// Extract the multicast delegate property from Children[0]
		TSharedPtr<FDecompiledExpr> DelegateExpr = Expr->Children[0];
		FProperty* DelegateProp = nullptr;
		bool bSelfContext = false;
		TSharedPtr<FDecompiledExpr> TargetObjectExpr;

		if (DelegateExpr)
		{
			if (DelegateExpr->Token == EX_InstanceVariable && DelegateExpr->PropertyRef)
			{
				// Self delegate — property is directly on this Blueprint
				DelegateProp = CastField<FProperty>(DelegateExpr->PropertyRef);
				bSelfContext = true;
			}
			else if (DelegateExpr->Token == EX_Context || DelegateExpr->Token == EX_Context_FailSilent)
			{
				// Delegate on another object (e.g., InventorySystem.OnInventoryItemRemoved)
				TargetObjectExpr = DelegateExpr->ContextObject;
				// Try FieldRef first (R-value property from EX_Context)
				if (DelegateExpr->FieldRef)
				{
					DelegateProp = CastField<FProperty>(DelegateExpr->FieldRef);
				}
				// Fall back to inner expression's PropertyRef
				if (!DelegateProp && DelegateExpr->Children.Num() > 0 &&
					DelegateExpr->Children[0] && DelegateExpr->Children[0]->PropertyRef)
				{
					DelegateProp = CastField<FProperty>(DelegateExpr->Children[0]->PropertyRef);
				}
			}
		}

		if (!DelegateProp)
		{
			Warn(FString::Printf(TEXT("Could not extract delegate property for %s at offset 0x%04X"),
				Expr->Token == EX_AddMulticastDelegate ? TEXT("AddMulticastDelegate") : TEXT("RemoveMulticastDelegate"),
				Expr->StartOffset));
			return nullptr;
		}

		UClass* OwnerClass = DelegateProp->GetOwnerClass();

		// Create the appropriate node
		UK2Node_BaseMCDelegate* DelegateNode = nullptr;
		if (Expr->Token == EX_AddMulticastDelegate)
		{
			DelegateNode = NewObject<UK2Node_AddDelegate>(Graph);
		}
		else
		{
			DelegateNode = NewObject<UK2Node_RemoveDelegate>(Graph);
		}

		DelegateNode->CreateNewGuid();
		DelegateNode->SetFromProperty(DelegateProp, bSelfContext, OwnerClass);
		DelegateNode->NodePosX = X;
		DelegateNode->NodePosY = Y;
		DelegateNode->AllocateDefaultPins();
		Graph->AddNode(DelegateNode, false, false);

		// Wire target object to Self pin (if not self context)
		if (!bSelfContext && TargetObjectExpr)
		{
			UEdGraphPin* SelfPin = FindDataPin(DelegateNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
			if (SelfPin)
			{
				int32 DataX = X - 200;
				int32 DataY = Y + DataNodeOffsetY;
				UEdGraphPin* TargetPin = ResolveDataExpr(Graph, TargetObjectExpr, DataX, DataY);
				ForceConnect(TargetPin, SelfPin);
			}
		}

		// Wire delegate to Delegate pin
		UEdGraphPin* DelegatePin = DelegateNode->GetDelegatePin();
		if (DelegatePin && Expr->Children[1])
		{
			int32 DataX = X - 200;
			int32 DataY = Y + DataNodeOffsetY + NodeSpacingY / 2;
			UEdGraphPin* DelegateSourcePin = ResolveDataExpr(Graph, Expr->Children[1], DataX, DataY);
			ForceConnect(DelegateSourcePin, DelegatePin);
		}

		return DelegateNode;
	}

	// EX_ClearMulticastDelegate → UK2Node_ClearDelegate
	// Clears all bindings from a multicast delegate property.
	//   Children[0] = multicast delegate expression (property reference)
	case EX_ClearMulticastDelegate:
	{
		if (Expr->Children.Num() < 1) return nullptr;

		// Extract the multicast delegate property (same logic as Add/Remove)
		TSharedPtr<FDecompiledExpr> DelegateExpr = Expr->Children[0];
		FProperty* DelegateProp = nullptr;
		bool bSelfContext = false;
		TSharedPtr<FDecompiledExpr> TargetObjectExpr;

		if (DelegateExpr)
		{
			if (DelegateExpr->Token == EX_InstanceVariable && DelegateExpr->PropertyRef)
			{
				DelegateProp = CastField<FProperty>(DelegateExpr->PropertyRef);
				bSelfContext = true;
			}
			else if (DelegateExpr->Token == EX_Context || DelegateExpr->Token == EX_Context_FailSilent)
			{
				TargetObjectExpr = DelegateExpr->ContextObject;
				if (DelegateExpr->FieldRef)
				{
					DelegateProp = CastField<FProperty>(DelegateExpr->FieldRef);
				}
				if (!DelegateProp && DelegateExpr->Children.Num() > 0 &&
					DelegateExpr->Children[0] && DelegateExpr->Children[0]->PropertyRef)
				{
					DelegateProp = CastField<FProperty>(DelegateExpr->Children[0]->PropertyRef);
				}
			}
		}

		if (!DelegateProp)
		{
			Warn(FString::Printf(TEXT("Could not extract delegate property for ClearMulticastDelegate at offset 0x%04X"),
				Expr->StartOffset));
			return nullptr;
		}

		UClass* OwnerClass = DelegateProp->GetOwnerClass();

		UK2Node_ClearDelegate* ClearNode = NewObject<UK2Node_ClearDelegate>(Graph);
		ClearNode->CreateNewGuid();
		ClearNode->SetFromProperty(DelegateProp, bSelfContext, OwnerClass);
		ClearNode->NodePosX = X;
		ClearNode->NodePosY = Y;
		ClearNode->AllocateDefaultPins();
		Graph->AddNode(ClearNode, false, false);

		// Wire target object to Self pin (if not self context)
		if (!bSelfContext && TargetObjectExpr)
		{
			UEdGraphPin* SelfPin = FindDataPin(ClearNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
			if (SelfPin)
			{
				int32 DataX = X - 200;
				int32 DataY = Y + DataNodeOffsetY;
				UEdGraphPin* TargetPin = ResolveDataExpr(Graph, TargetObjectExpr, DataX, DataY);
				ForceConnect(TargetPin, SelfPin);
			}
		}

		return ClearNode;
	}

	// SWITCH VALUE
	case EX_SwitchValue:
	{
		// SwitchValue is a data expression that produces a value.
		// Forward to ResolveDataExpr which creates the UK2Node_Select.
		int32 DataX = X;
		int32 DataY = Y;
		ResolveDataExpr(Graph, Expr, DataX, DataY);
		return nullptr; // No exec node created — Select is pure
	}

	default:
		// For anything else, log and skip
		UE_LOG(LogGraphBuilder, Verbose, TEXT("Skipping expression token 0x%02X at offset 0x%04X"),
			(uint8)Expr->Token, Expr->StartOffset);
		return nullptr;
	}
}

// Data Expression Resolution (sub-expressions → output pins)

UEdGraphPin* FGraphBuilder::ResolveDataExpr(
	UEdGraph* Graph,
	TSharedPtr<FDecompiledExpr> Expr,
	int32& X, int32& Y)
{
	// Iterative loop: tail-recursive cases (EX_Context, EX_StructMemberContext,
	// EX_InterfaceContext, EX_VirtualFunction) update Expr and 'continue' instead
	// of recursing, avoiding stack overflow on deeply nested expression trees.
	static const int32 MaxTailIterations = 512;
	int32 TailIterCount = 0;
	for (;;)
	{
	if (!Expr || !Graph) return nullptr;
	if (++TailIterCount > MaxTailIterations)
	{
		Warn(TEXT("ResolveDataExpr: exceeded maximum iteration depth (possible cycle in expression tree)"));
		return nullptr;
	}

	switch (Expr->Token)
	{
		// VARIABLE GET
	case EX_InstanceVariable:
	{
		if (!Expr->PropertyRef) return nullptr;

		// ── Iterative ContextObject chain processing ──────────────────────
		// For deeply nested property access chains like A.B.C.D.E, each level
		// is an EX_InstanceVariable with a ContextObject pointing to the next
		// outer level. Processing these recursively causes stack overflow on
		// large blueprints (e.g. player controllers). Instead, we collect the
		// chain into an array and process it iteratively.
		struct FVarChainEntry
		{
			UK2Node_VariableGet* Node;
			UEdGraphPin* OutputPin;
			UEdGraphPin* SelfPin; // target/self input pin (null if no context)
		};
		TArray<FVarChainEntry, TInlineAllocator<32>> Chain;
		TSharedPtr<FDecompiledExpr> CurrentExpr = Expr;
		TSharedPtr<FDecompiledExpr> RemainingContext; // non-InstanceVariable at chain bottom
		int32 NodeX = X;

		// Track visited expressions to detect cycles created by EX_Context
		// chaining (it mutates ContextObject pointers in place, which can
		// create circular references when expression nodes are shared).
		TSet<FDecompiledExpr*> Visited;

		while (CurrentExpr)
		{
			// Cycle detection: if we've already processed this expression, stop.
			if (Visited.Contains(CurrentExpr.Get()))
			{
				Warn(TEXT("ResolveDataExpr: cycle detected in ContextObject chain, breaking"));
				break;
			}
			Visited.Add(CurrentExpr.Get());

			if (CurrentExpr->Token != EX_InstanceVariable || !CurrentExpr->PropertyRef)
			{
				// Hit a non-InstanceVariable expression at the bottom of the chain;
				// we'll resolve it with a single (shallow) recursive call below.
				RemainingContext = CurrentExpr;
				break;
			}

			FName VarName = CurrentExpr->PropertyRef->GetFName();
			FString VarStr = VarName.ToString();

			// Multi-assigned Temp_ variables need dedicated TemporaryVariable nodes
			if (Chain.Num() == 0 && MultiAssignedTempVars.Contains(VarName))
			{
				if (UEdGraphPin** CachedPin = MultiAssignedTempPins.Find(VarName))
				{
					if (*CachedPin) return *CachedPin;
				}
				UK2Node_TemporaryVariable* TempNode = NewObject<UK2Node_TemporaryVariable>(Graph);
				TempNode->CreateNewGuid();
				FEdGraphPinType VarType;
				GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(CurrentExpr->PropertyRef, VarType);
				TempNode->VariableType = VarType;
				TempNode->NodePosX = NodeX;
				TempNode->NodePosY = Y;
				TempNode->AllocateDefaultPins();
				Graph->AddNode(TempNode, false, false);
				UEdGraphPin* OutPin = FindFirstDataOutputPin(TempNode);
				if (OutPin) MultiAssignedTempPins.Add(VarName, OutPin);
				return OutPin;
			}

			// Determine if this is a compiler-generated name (persistent frame variable)
			// vs a real class property. Real properties (like "HideLocation") must always
			// create VariableGet nodes. Only compiler-generated names should check
			// LocalVarToPinMap, because event parameter names can collide with instance
			// variable names (e.g. event param "HideLocation" (Vector) vs instance var
			// "HideLocation" (Object) — resolving to the wrong pin causes type mismatches).
			bool bIsCompilerGenerated = VarStr.Contains(TEXT("K2Node_")) ||
				VarStr.Contains(TEXT("CallFunc_")) || VarStr.StartsWith(TEXT("Temp_"));

			// Check if this variable was produced by a previous function call
			// (only for compiler-generated names to avoid name collisions)
			if (bIsCompilerGenerated)
			{
				UEdGraphPin** FoundPin = LocalVarToPinMap.Find(VarName);
				if (!FoundPin) FoundPin = PersistentFrameVarMap.Find(VarName);
				if (FoundPin && *FoundPin)
				{
					// Pre-existing pin — if we already created nodes, wire it as the
					// deepest context output; otherwise just return it directly.
					if (Chain.Num() > 0 && Chain.Last().SelfPin)
					{
						ForceConnect(*FoundPin, Chain.Last().SelfPin);
					}
					else
					{
						return *FoundPin;
					}
					break;
				}
			}

			// Suppress ghost nodes for compiler temporaries not found in the map.
			if (bIsCompilerGenerated)
			{
				// Helper lambda: check if Str ends with Key at a word boundary
				// (preceded by '_' or at the start of string). Prevents false
				// positives like "K2Node_DynamicCast_AsAttachmentComponent" matching
				// key "AttachmentComponent" (the 's' before 'A' is not '_').
				auto EndsWithAtBoundary = [](const FString& Str, const FString& Key) -> bool
				{
					if (!Str.EndsWith(Key)) return false;
					int32 MatchStart = Str.Len() - Key.Len();
					return MatchStart == 0 || Str[MatchStart - 1] == TEXT('_');
				};

				// Pass 1: EndsWith match at word boundary, prefer longest key
				UEdGraphPin* MatchedPin = nullptr;
				int32 BestKeyLen = 0;
				for (auto& KV : LocalVarToPinMap)
				{
					if (KV.Value)
					{
						FString KeyStr = KV.Key.ToString();
						if (KeyStr.Len() > BestKeyLen && EndsWithAtBoundary(VarStr, KeyStr))
						{
							MatchedPin = KV.Value;
							BestKeyLen = KeyStr.Len();
						}
					}
				}
				// Pass 2: strip trailing _N disambiguation suffix and try again
				if (!MatchedPin)
				{
					int32 LastUnderscore = VarStr.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (LastUnderscore != INDEX_NONE)
					{
						FString Suffix = VarStr.Mid(LastUnderscore + 1);
						if (Suffix.Len() > 0 && Suffix.Len() <= 3 && Suffix.IsNumeric())
						{
							FString StrippedVarStr = VarStr.Left(LastUnderscore);
							BestKeyLen = 0;
							for (auto& KV : LocalVarToPinMap)
							{
								if (KV.Value)
								{
									FString KeyStr = KV.Key.ToString();
									if (KeyStr.Len() > BestKeyLen && EndsWithAtBoundary(StrippedVarStr, KeyStr))
									{
										MatchedPin = KV.Value;
										BestKeyLen = KeyStr.Len();
									}
								}
							}
							if (MatchedPin)
							{
								// Cache the mapping
								LocalVarToPinMap.Add(VarName, MatchedPin);
							}
						}
					}
				}
				if (MatchedPin)
				{
					if (Chain.Num() > 0 && Chain.Last().SelfPin)
					{
						ForceConnect(MatchedPin, Chain.Last().SelfPin);
					}
					else
					{
						return MatchedPin;
					}
					break;
				}
				// Compiler temp with no match — stop chain
				if (Chain.Num() == 0) return nullptr;
				break;
			}

			// Create the UK2Node_VariableGet for this level
			UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
			GetNode->CreateNewGuid();

			if (CurrentExpr->ContextObject)
			{
				UClass* OwnerClass = CurrentExpr->PropertyRef->GetOwnerClass();
				if (OwnerClass)
				{
					GetNode->VariableReference.SetExternalMember(VarName, OwnerClass);
				}
				else
				{
					GetNode->VariableReference.SetSelfMember(VarName);
				}
			}
			else
			{
				GetNode->VariableReference.SetSelfMember(VarName);
			}

			GetNode->NodePosX = NodeX;
			GetNode->NodePosY = Y;
			GetNode->AllocateDefaultPins();

			// Fix output pin types from PropertyRef
			UEdGraphPin* VarOutPin = FindFirstDataOutputPin(GetNode);
			if (VarOutPin && CurrentExpr->PropertyRef)
			{
				FEdGraphPinType CorrectType;
				GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(CurrentExpr->PropertyRef, CorrectType);
				if (VarOutPin->PinType.PinCategory != CorrectType.PinCategory ||
					VarOutPin->PinType.PinSubCategoryObject != CorrectType.PinSubCategoryObject)
				{
					VarOutPin->PinType = CorrectType;
				}
			}
			else if (!VarOutPin && CurrentExpr->PropertyRef)
			{
				FEdGraphPinType CorrectType;
				GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(CurrentExpr->PropertyRef, CorrectType);
				VarOutPin = GetNode->CreatePin(EGPD_Output, CorrectType, VarName);
			}

			Graph->AddNode(GetNode, false, false);

			FVarChainEntry Entry;
			Entry.Node = GetNode;
			Entry.OutputPin = FindFirstDataOutputPin(GetNode);
			Entry.SelfPin = CurrentExpr->ContextObject
				? FindDataPin(GetNode, EGPD_Input, UEdGraphSchema_K2::PN_Self)
				: nullptr;
			Chain.Add(Entry);

			NodeX -= 200;

			if (!CurrentExpr->ContextObject)
				break;

			CurrentExpr = CurrentExpr->ContextObject;
		}

		// Wire the chain: each deeper node's output feeds the shallower node's self pin.
		// Chain[0] is the shallowest (original Expr), Chain.Last() is the deepest.
		for (int32 i = Chain.Num() - 1; i > 0; --i)
		{
			if (Chain[i].OutputPin && Chain[i - 1].SelfPin)
			{
				ForceConnect(Chain[i].OutputPin, Chain[i - 1].SelfPin);
			}
		}

		// If the chain bottomed out on a non-EX_InstanceVariable expression,
		// resolve it with a (now shallow) recursive call and wire it in.
		if (RemainingContext && Chain.Num() > 0 && Chain.Last().SelfPin)
		{
			int32 CtxX = NodeX;
			int32 CtxY = Y;
			UEdGraphPin* BottomOut = ResolveDataExpr(Graph, RemainingContext, CtxX, CtxY);
			ForceConnect(BottomOut, Chain.Last().SelfPin);
		}

		X -= NodeSpacingX / 2;
		return Chain.Num() == 0 ? nullptr : Chain[0].OutputPin;
	}

	case EX_LocalVariable:
	case EX_DefaultVariable:
	case EX_LocalOutVariable:
	{
		if (!Expr->PropertyRef) return nullptr;

		FName VarName = Expr->PropertyRef->GetFName();

		// Multi-assigned Temp_ variables (loop counters, array indices, etc.) need
		// dedicated TemporaryVariable nodes — they can't be optimized away via
		// LocalVarToPinMap because their value changes at runtime. All reads of the
		// same variable share one TemporaryVariable node.
		if (MultiAssignedTempVars.Contains(VarName))
		{
			if (UEdGraphPin** CachedPin = MultiAssignedTempPins.Find(VarName))
			{
				if (*CachedPin) return *CachedPin;
			}
			// Create a new TemporaryVariable node for this multi-assigned var
			UK2Node_TemporaryVariable* TempNode = NewObject<UK2Node_TemporaryVariable>(Graph);
			TempNode->CreateNewGuid();
			FEdGraphPinType VarType;
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Expr->PropertyRef, VarType);
			TempNode->VariableType = VarType;
			TempNode->NodePosX = X;
			TempNode->NodePosY = Y;
			TempNode->AllocateDefaultPins();
			Graph->AddNode(TempNode, false, false);
			UEdGraphPin* OutPin = FindFirstDataOutputPin(TempNode);
			if (OutPin) MultiAssignedTempPins.Add(VarName, OutPin);
			X -= NodeSpacingX / 2;
			return OutPin;
		}

		// Check if this variable was produced by a previous function call or is a function parameter
		if (UEdGraphPin** FoundPin = LocalVarToPinMap.Find(VarName))
		{
			if (*FoundPin)
			{
				return *FoundPin;
			}
		}

		// Fallback: check persistent frame variables from other events
		if (UEdGraphPin** PersistPin = PersistentFrameVarMap.Find(VarName))
		{
			if (*PersistPin)
			{
				return *PersistPin;
			}
		}

		// ── MakeStruct: create UK2Node_MakeStruct for accumulated struct member writes ──
		// When the compiler inlines a MakeStruct, it creates a local struct variable
		// (K2Node_MakeStruct_*) and writes members via EX_StructMemberContext. Those
		// writes are accumulated in StructMemberAssignMap. When the variable is consumed
		// here, we create the actual MakeStruct node.
		if (FStructMemberWrite* StructWrite = StructMemberAssignMap.Find(VarName))
		{
			if (StructWrite->StructType)
			{
				// Check if we already created a MakeStruct node for this variable
				if (UEdGraphPin** CachedPin = LocalVarToPinMap.Find(VarName))
				{
					if (*CachedPin) return *CachedPin;
				}

				UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(Graph);
				MakeNode->CreateNewGuid();
				MakeNode->StructType = StructWrite->StructType;
				MakeNode->bMadeAfterOverridePinRemoval = true;
				MakeNode->NodePosX = X;
				MakeNode->NodePosY = Y;
				MakeNode->AllocateDefaultPins();
				Graph->AddNode(MakeNode, false, false);

				// Apply accumulated member values to input pins
				for (auto& MemberPair : StructWrite->MemberValues)
				{
					UEdGraphPin* MemberPin = MakeNode->FindPin(MemberPair.Key, EGPD_Input);
					if (!MemberPin)
					{
						// Try friendly name matching — pin names might use short names
						for (UEdGraphPin* Pin : MakeNode->Pins)
						{
							if (Pin->Direction == EGPD_Input &&
								Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
								Pin->GetFName() == MemberPair.Key)
							{
								MemberPin = Pin;
								break;
							}
						}
					}

					if (MemberPin && MemberPair.Value)
					{
						TSharedPtr<FDecompiledExpr> ValExpr = MemberPair.Value;
						if (ValExpr->IsLiteral())
						{
							if (ValExpr->Token == EX_True)
								MemberPin->DefaultValue = TEXT("true");
							else if (ValExpr->Token == EX_False)
								MemberPin->DefaultValue = TEXT("false");
							else if (ValExpr->Token == EX_NoObject)
								MemberPin->DefaultValue = TEXT("");
							else if (ValExpr->Token == EX_ObjectConst && ValExpr->ObjectRef)
								MemberPin->DefaultObject = ValExpr->ObjectRef;
							else
							{
								// Enum-aware resolution
								bool bEnumResolved = false;
								if ((ValExpr->Token == EX_ByteConst || ValExpr->Token == EX_IntConst) &&
									MemberPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
									MemberPin->PinType.PinSubCategoryObject.IsValid())
								{
									UEnum* PinEnum = Cast<UEnum>(MemberPin->PinType.PinSubCategoryObject.Get());
									if (PinEnum)
									{
										int64 Val = (ValExpr->Token == EX_ByteConst) ? ValExpr->ByteValue : ValExpr->IntValue;
										FString EnumName = PinEnum->GetNameStringByValue(Val);
										if (!EnumName.IsEmpty())
										{
											MemberPin->DefaultValue = EnumName;
											bEnumResolved = true;
										}
									}
								}
								if (!bEnumResolved)
								{
									MemberPin->DefaultValue = ValExpr->GetLiteralAsString();
								}
							}
						}
						else
						{
							// Non-literal source — resolve to a pin and wire it
							int32 DataX = X - 200;
							int32 DataY = Y + DataNodeOffsetY;
							UEdGraphPin* SourcePin = ResolveDataExpr(Graph, ValExpr, DataX, DataY);
							if (SourcePin)
							{
								ForceConnect(SourcePin, MemberPin);
							}
						}
					}
				}

				// Find the output pin (named after the struct type)
				UEdGraphPin* OutputPin = MakeNode->FindPin(StructWrite->StructType->GetFName(), EGPD_Output);
				if (!OutputPin)
				{
					OutputPin = FindFirstDataOutputPin(MakeNode);
				}

				if (OutputPin)
				{
					LocalVarToPinMap.Add(VarName, OutputPin);
				}

				UE_LOG(LogGraphBuilder, Log, TEXT("Created MakeStruct node for '%s' (type=%s, %d members)"),
					*VarName.ToString(), *StructWrite->StructType->GetName(),
					StructWrite->MemberValues.Num());

				X -= NodeSpacingX;
				return OutputPin;
			}
		}

		// Suppress ghost nodes for compiler temporaries not found in the map.
		// But first check if this is a persistent frame variable wrapping an
		// event/function parameter (e.g., K2Node_Event_NewController for the
		// NewController parameter). Match the suffix against registered names.
		FString VarStr = VarName.ToString();
		if (VarStr.Contains(TEXT("K2Node_")) || VarStr.Contains(TEXT("CallFunc_")) ||
			VarStr.StartsWith(TEXT("Temp_")))
		{
			// Helper lambda: check EndsWith at a word boundary (preceded by '_')
			auto EndsWithAtBoundary = [](const FString& Str, const FString& Key) -> bool
			{
				if (!Str.EndsWith(Key)) return false;
				int32 MatchStart = Str.Len() - Key.Len();
				return MatchStart == 0 || Str[MatchStart - 1] == TEXT('_');
			};

			// Pass 1: EndsWith match at word boundary, prefer longest key
			UEdGraphPin* BestMatch = nullptr;
			int32 BestKeyLen = 0;
			for (auto& KV : LocalVarToPinMap)
			{
				if (KV.Value)
				{
					FString KeyStr = KV.Key.ToString();
					if (KeyStr.Len() > BestKeyLen && EndsWithAtBoundary(VarStr, KeyStr))
					{
						BestMatch = KV.Value;
						BestKeyLen = KeyStr.Len();
					}
				}
			}
			if (BestMatch)
			{
				return BestMatch;
			}
			// Pass 2: strip trailing _N disambiguation suffix and try again
			// (e.g., K2Node_Event_SaveDataObject_2 → K2Node_Event_SaveDataObject → matches SaveDataObject)
			int32 LastUnderscore = VarStr.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (LastUnderscore != INDEX_NONE)
			{
				FString Suffix = VarStr.Mid(LastUnderscore + 1);
				if (Suffix.Len() > 0 && Suffix.Len() <= 3 && Suffix.IsNumeric())
				{
					FString StrippedVarStr = VarStr.Left(LastUnderscore);
					BestMatch = nullptr;
					BestKeyLen = 0;
					for (auto& KV : LocalVarToPinMap)
					{
						if (KV.Value)
						{
							FString KeyStr = KV.Key.ToString();
							if (KeyStr.Len() > BestKeyLen && EndsWithAtBoundary(StrippedVarStr, KeyStr))
							{
								BestMatch = KV.Value;
								BestKeyLen = KeyStr.Len();
							}
						}
					}
					if (BestMatch)
					{
						// Cache the mapping so subsequent lookups are direct
						LocalVarToPinMap.Add(VarName, BestMatch);
						return BestMatch;
					}
				}
			}
			return nullptr;
		}

		// Determine if this is a real instance/member variable or a true local variable.
		// EX_InstanceVariable and EX_DefaultVariable are always member variables.
		// EX_LocalVariable and EX_LocalOutVariable are function-scoped locals
		// (out-params, function results, etc.) but may occasionally reference
		// member variables if the decompiler tagged them ambiguously.
		bool bIsInstanceVariable = false;
		if (Expr->Token == EX_DefaultVariable)
		{
			bIsInstanceVariable = true;
		}
		else if (Expr->Token == EX_LocalVariable || Expr->Token == EX_LocalOutVariable)
		{
			// Check if this name actually exists as an instance variable on the class.
			// If not, creating SetSelfMember would produce "not found in <ClassName>" errors.
			if (CurrentOriginalClass)
			{
				bIsInstanceVariable = (FindFProperty<FProperty>(CurrentOriginalClass, VarName) != nullptr);
			}
		}

		if (bIsInstanceVariable)
		{
			// Instance/member variable — create VariableGet with SetSelfMember
			UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
			GetNode->CreateNewGuid();
			GetNode->VariableReference.SetSelfMember(VarName);
			GetNode->NodePosX = X;
			GetNode->NodePosY = Y;
			GetNode->AllocateDefaultPins();

			UEdGraphPin* VarOutPin = FindFirstDataOutputPin(GetNode);
			if (VarOutPin && Expr->PropertyRef)
			{
				FEdGraphPinType CorrectType;
				GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Expr->PropertyRef, CorrectType);
				if (VarOutPin->PinType.PinCategory != CorrectType.PinCategory ||
					VarOutPin->PinType.PinSubCategoryObject != CorrectType.PinSubCategoryObject)
				{
					VarOutPin->PinType = CorrectType;
				}
			}
			else if (!VarOutPin && Expr->PropertyRef)
			{
				FEdGraphPinType CorrectType;
				GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Expr->PropertyRef, CorrectType);
				VarOutPin = GetNode->CreatePin(EGPD_Output, CorrectType, VarName);
			}

			Graph->AddNode(GetNode, false, false);
			X -= NodeSpacingX / 2;
			return VarOutPin ? VarOutPin : FindFirstDataOutputPin(GetNode);
		}
		else
		{
			// True local variable (function out-param, intermediate result, etc.)
			// not yet registered in LocalVarToPinMap. Don't create SetSelfMember
			// VariableGet — the variable doesn't exist as a class member and would
			// cause "not found in <ClassName>" errors.
			// Create a TemporaryVariable node with the correct type from PropertyRef.
			Warn(FString::Printf(TEXT("Local variable '%s' not found in LocalVarToPinMap — creating typed local node"),
				*VarName.ToString()));

			UK2Node_TemporaryVariable* TempNode = NewObject<UK2Node_TemporaryVariable>(Graph);
			TempNode->CreateNewGuid();

			FEdGraphPinType VarType;
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Expr->PropertyRef, VarType);
			TempNode->VariableType = VarType;

			TempNode->NodePosX = X;
			TempNode->NodePosY = Y;
			TempNode->AllocateDefaultPins();
			Graph->AddNode(TempNode, false, false);

			UEdGraphPin* OutPin = FindFirstDataOutputPin(TempNode);
			if (OutPin)
			{
				// Register so subsequent references to this variable wire to the same pin
				LocalVarToPinMap.Add(VarName, OutPin);
			}

			X -= NodeSpacingX / 2;
			return OutPin;
		}
	}

	// SELF
	case EX_Self:
	{
		UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
		SelfNode->CreateNewGuid();
		SelfNode->NodePosX = X;
		SelfNode->NodePosY = Y;
		SelfNode->AllocateDefaultPins();
		Graph->AddNode(SelfNode, false, false);

		X -= NodeSpacingX / 2;
		return FindFirstDataOutputPin(SelfNode);
	}

	// FUNCTION CALLS AS DATA (pure functions, or return values of impure calls)
	case EX_FinalFunction:
	case EX_LocalFinalFunction:
	case EX_CallMath:
	{
		if (!Expr->FunctionRef) return nullptr;

		// ── Subsystem getter detection ──────────────────────────────────
		// K2Node_GetSubsystem compiles to USubsystemBlueprintLibrary::GetGameInstanceSubsystem
		// which returns UGameInstanceSubsystem* (base type). The original K2Node returns the
		// specific subclass (e.g. FNAFInventorySystem*) via CustomClass. We create the normal
		// CallFunction node, then patch the ReturnValue pin type to the specific subclass.
		FString FuncName = Expr->FunctionRef->GetName();
		bool bIsSubsystemGetter = FuncName.Contains(TEXT("GetGameInstanceSubsystem")) ||
			FuncName.Contains(TEXT("GetWorldSubsystem")) ||
			FuncName.Contains(TEXT("GetLocalPlayerSubsystem")) ||
			FuncName.Contains(TEXT("GetEngineSubsystem"));

		UClass* SubsystemClass = nullptr;
		if (bIsSubsystemGetter)
		{
			// Extract the subsystem class from arguments (EX_ObjectConst pointing to a UClass)
			for (const TSharedPtr<FDecompiledExpr>& Arg : Expr->Children)
			{
				if (Arg && Arg->Token == EX_ObjectConst && Arg->ObjectRef)
				{
					SubsystemClass = Cast<UClass>(Arg->ObjectRef);
					if (SubsystemClass) break;
				}
			}
		}
		// ── End subsystem class extraction ──────────────────────────────

		// Use UK2Node_CallArrayFunction for array library functions (same as exec handler)
		bool bIsArrayLibFunc = false;
		{
			UClass* FuncClass = Expr->FunctionRef->GetOwnerClass();
			if (FuncClass && FuncClass->GetName().Contains(TEXT("KismetArrayLibrary")))
			{
				bIsArrayLibFunc = true;
			}
		}
		UK2Node_CallFunction* CallNode = bIsArrayLibFunc
			? NewObject<UK2Node_CallArrayFunction>(Graph)
			: NewObject<UK2Node_CallFunction>(Graph);
		CallNode->CreateNewGuid();

		// Only redirect to self-member for functions in the same class hierarchy
		UClass* FuncOwner = Expr->FunctionRef->GetOwnerClass();
		bool bIsBPGCFunc = FuncOwner && Cast<UBlueprintGeneratedClass>(FuncOwner) != nullptr;
		bool bIsSelfBPGCFunc = bIsBPGCFunc && CurrentOriginalClass && CurrentOriginalClass->IsChildOf(FuncOwner);

		// Also check interface implementation (same logic as exec path)
		if (!bIsSelfBPGCFunc && bIsBPGCFunc && CurrentOriginalClass && !Expr->ContextObject)
		{
			if (CurrentOriginalClass->ImplementsInterface(FuncOwner))
			{
				bIsSelfBPGCFunc = true;
			}
			else
			{
				for (const FImplementedInterface& Iface : CurrentOriginalClass->Interfaces)
				{
					if (Iface.Class == FuncOwner ||
						(Iface.Class && Iface.Class->GetFName() == FuncOwner->GetFName()))
					{
						bIsSelfBPGCFunc = true;
						break;
					}
				}
			}
		}

		// Avoid SetFromFunction — crashes for cooked BPGC functions (SetGivenSelfScope
		// dereferences GetOwnerClass() without null check). Set properties manually.
		CallNode->bIsPureFunc = Expr->FunctionRef->HasAnyFunctionFlags(FUNC_BlueprintPure);
		CallNode->bIsConstFunc = Expr->FunctionRef->HasAnyFunctionFlags(FUNC_Const);
		// Note: DetermineWantsEnumToExecExpansion is private — skip it.

		if (bIsSelfBPGCFunc)
		{
			// Use SetExternalMember for AllocateDefaultPins (same fix as exec path)
			CallNode->FunctionReference.SetExternalMember(Expr->FunctionRef->GetFName(), FuncOwner);
		}
		else
		{
			UClass* RefClass = FuncOwner ? FuncOwner : CurrentOriginalClass;
			CallNode->FunctionReference.SetExternalMember(Expr->FunctionRef->GetFName(), RefClass);
		}

		CallNode->NodePosX = X;
		CallNode->NodePosY = Y;
		CallNode->AllocateDefaultPins();
		FixStructPinDefaults(CallNode);
		Graph->AddNode(CallNode, false, false);

		if (bIsSelfBPGCFunc)
		{
			// Switch to self-member for compilation
			CallNode->FunctionReference.SetSelfMember(Expr->FunctionRef->GetFName());

			// Remove Target/Self pin — self-member calls don't need it
			UEdGraphPin* SelfPin = FindDataPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
			if (SelfPin)
			{
				CallNode->Pins.Remove(SelfPin);
				SelfPin->MarkPendingKill();
			}
		}

		// Wire arguments recursively
		int32 ArgX = X - 200;
		int32 ArgY = Y;
		int32 ArgIndex = 0;

		for (const TSharedPtr<FDecompiledExpr>& Arg : Expr->Children)
		{
			if (!Arg) { ArgIndex++; continue; }

			FName PinName = GetPinNameForFunctionParam(Expr->FunctionRef, ArgIndex);
			UEdGraphPin* ArgPin = nullptr;

			if (PinName != NAME_None)
			{
				ArgPin = FindDataPin(CallNode, EGPD_Input, PinName);
			}

			if (!ArgPin)
			{
				int32 DataPinIdx = 0;
				for (UEdGraphPin* Pin : CallNode->Pins)
				{
					if (Pin->Direction == EGPD_Input &&
						Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
						Pin->PinName != UEdGraphSchema_K2::PN_Self)
					{
						if (DataPinIdx == ArgIndex)
						{
							ArgPin = Pin;
							break;
						}
						DataPinIdx++;
					}
				}
			}

			if (ArgPin)
			{
				if (Arg->IsLiteral())
				{
					// Check if pin expects an enum — resolve name instead of raw int
					bool bEnumResolved = false;
					if ((Arg->Token == EX_ByteConst || Arg->Token == EX_IntConst ||
						Arg->Token == EX_IntConstByte || Arg->Token == EX_IntZero || Arg->Token == EX_IntOne) &&
						ArgPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
						ArgPin->PinType.PinSubCategoryObject.IsValid())
					{
						UEnum* PinEnum = Cast<UEnum>(ArgPin->PinType.PinSubCategoryObject.Get());
						if (PinEnum)
						{
							int64 Val = (Arg->Token == EX_ByteConst) ? Arg->ByteValue : Arg->IntValue;
							FString EnumName = PinEnum->GetNameStringByValue(Val);
							if (!EnumName.IsEmpty())
							{
								ArgPin->DefaultValue = EnumName;
								bEnumResolved = true;
							}
						}
					}
					if (!bEnumResolved)
					{
						ArgPin->DefaultValue = Arg->GetLiteralAsString();
					}
					if (Arg->Token == EX_ObjectConst && Arg->ObjectRef)
					{
						ArgPin->DefaultObject = Arg->ObjectRef;
					}
				}
				else if (ApplyTempLiteralToPin(Arg, ArgPin))
				{
					// Temp_ literal applied as pin default
				}
				else
				{
					UEdGraphPin* ArgOut = ResolveDataExpr(Graph, Arg, ArgX, ArgY);
					if (ArgOut)
					{
						TryConnect(ArgOut, ArgPin);
					}
					else if (Arg->PropertyRef &&
						(Arg->Token == EX_LocalVariable || Arg->Token == EX_LocalOutVariable))
					{
						// Defer: variable not yet in map (backward-jump pattern)
						DeferredDataWires.Add(TPair<UEdGraphPin*, FName>(
							ArgPin, Arg->PropertyRef->GetFName()));
					}
					ArgY += NodeSpacingY / 3;
				}
			}
			ArgIndex++;
		}

		// Wire context object if present (skip world context objects)
		if (Expr->ContextObject && Expr->ContextObject->Token != EX_ObjectConst)
		{
			UEdGraphPin* TargetPin = FindDataPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);

			// Library functions have no Self pin — try TargetArray, then first data input
			if (!TargetPin)
			{
				TargetPin = FindDataPin(CallNode, EGPD_Input, TEXT("TargetArray"));
			}
			if (!TargetPin)
			{
				for (UEdGraphPin* Pin : CallNode->Pins)
				{
					if (Pin->Direction == EGPD_Input &&
						Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
						Pin->LinkedTo.Num() == 0)
					{
						TargetPin = Pin;
						break;
					}
				}
			}

			if (TargetPin)
			{
				UEdGraphPin* CtxOut = ResolveDataExpr(Graph, Expr->ContextObject, ArgX, ArgY);
				ForceConnect(CtxOut, TargetPin);
			}
		}

		X -= NodeSpacingX / 2;

		// ── Array operation wildcard pin resolution ─────────────────────
		// UKismetArrayLibrary functions (Array_Get, Array_Length, Array_Contains, etc.)
		// have wildcard pins. Resolve them from the array's element type so downstream
		// connections (like Cast nodes) get proper type info instead of wildcard.
		{
			UClass* FuncClass = Expr->FunctionRef->GetOwnerClass();
			if (FuncClass && FuncClass->GetName().Contains(TEXT("KismetArrayLibrary")))
			{
				// Try to determine element type from the context object (the array itself)
				FEdGraphPinType ElemType;
				bool bFoundElemType = false;

				if (Expr->ContextObject && Expr->ContextObject->PropertyRef)
				{
					FArrayProperty* ArrayProp = CastField<FArrayProperty>(Expr->ContextObject->PropertyRef);
					if (ArrayProp && ArrayProp->Inner)
					{
						GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(ArrayProp->Inner, ElemType);
						bFoundElemType = true;
					}
				}

				// Also check the Target/TargetArray pin's connection type
				if (!bFoundElemType)
				{
					// Try both "TargetArray" (FName) and "Target Array" (display name)
					UEdGraphPin* ArrayPin = FindDataPin(CallNode, EGPD_Input, TEXT("TargetArray"));
					if (!ArrayPin || ArrayPin->LinkedTo.Num() == 0)
					{
						ArrayPin = FindDataPin(CallNode, EGPD_Input, TEXT("Target Array"));
					}
					if (!ArrayPin || ArrayPin->LinkedTo.Num() == 0)
					{
						// Last resort: find any connected input pin with Array container type
						for (UEdGraphPin* Pin : CallNode->Pins)
						{
							if (Pin->Direction == EGPD_Input &&
								Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
								Pin->LinkedTo.Num() > 0 &&
								(Pin->PinType.ContainerType == EPinContainerType::Array ||
									(Pin->LinkedTo[0]->PinType.ContainerType == EPinContainerType::Array)))
							{
								ArrayPin = Pin;
								break;
							}
						}
					}
					if (ArrayPin && ArrayPin->LinkedTo.Num() > 0)
					{
						FEdGraphPinType ConnectedType = ArrayPin->LinkedTo[0]->PinType;
						if (ConnectedType.ContainerType == EPinContainerType::Array)
						{
							ElemType = ConnectedType;
							ElemType.ContainerType = EPinContainerType::None;
							bFoundElemType = true;
						}
					}
				}

				if (bFoundElemType)
				{
					for (UEdGraphPin* Pin : CallNode->Pins)
					{
						if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
						{
							if (Pin->PinType.ContainerType == EPinContainerType::Array)
							{
								// Array-typed wildcard (e.g., TargetArray input)
								Pin->PinType = ElemType;
								Pin->PinType.ContainerType = EPinContainerType::Array;
							}
							else
							{
								// Scalar wildcard (e.g., Item output, Value input)
								Pin->PinType = ElemType;
							}
						}
					}
				}
			}
		}

		// ── Subsystem return type fix ──────────────────────────────────
		// GetGameInstanceSubsystem returns UGameInstanceSubsystem* (base class).
		// Insert a pure DynamicCast to the specific subclass so downstream
		// connections pass schema validation AND the type survives save/reload.
		// (PinSubCategoryObject patches don't survive because AllocateDefaultPins
		// recreates pins from the UFunction signature on reload.)
		if (bIsSubsystemGetter && SubsystemClass)
		{
			UEdGraphPin* BaseReturnPin = nullptr;
			for (UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin->Direction == EGPD_Output &&
					Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
				{
					BaseReturnPin = Pin;
					break;
				}
			}

			if (BaseReturnPin)
			{
				UClass* ReturnClass = Cast<UClass>(BaseReturnPin->PinType.PinSubCategoryObject.Get());
				if (ReturnClass && SubsystemClass != ReturnClass &&
					SubsystemClass->IsChildOf(ReturnClass))
				{
					// Insert pure DynamicCast: base class → specific subclass
					UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
					CastNode->CreateNewGuid();
					CastNode->TargetType = SubsystemClass;
					CastNode->SetPurity(true);
					CastNode->NodePosX = X + 200;
					CastNode->NodePosY = Y;
					Graph->AddNode(CastNode, false, false);
					CastNode->AllocateDefaultPins();

					// Wire: SubsystemGetter.ReturnValue → Cast.Object
					UEdGraphPin* CastObjPin = CastNode->GetCastSourcePin();
					if (CastObjPin)
					{
						BaseReturnPin->MakeLinkTo(CastObjPin);
					}

					// Return the typed cast output pin
					UEdGraphPin* CastResultPin = CastNode->GetCastResultPin();
					if (CastResultPin)
					{
						return CastResultPin;
					}
				}

				// If no cast needed (types match), return pin directly
				return BaseReturnPin;
			}
		}

		// Return the first data output pin (the return value)
		UEdGraphPin* ReturnPin = nullptr;
		for (UEdGraphPin* Pin : CallNode->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				if (Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
				{
					return Pin; // Prefer explicit ReturnValue pin
				}
				if (!ReturnPin)
				{
					ReturnPin = Pin;
				}
			}
		}
		return ReturnPin;
	}

	// CAST AS DATA
	case EX_DynamicCast:
	case EX_ObjToInterfaceCast:
	case EX_CrossInterfaceCast:
	case EX_InterfaceToObjCast:
	{
		if (!Expr->ClassRef) return nullptr;

		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
		CastNode->CreateNewGuid();
		CastNode->TargetType = Expr->ClassRef;
		CastNode->NodePosX = X;
		CastNode->NodePosY = Y;
		CastNode->AllocateDefaultPins();
		Graph->AddNode(CastNode, false, false);

		// Widen Object pin to UObject so ForceConnect doesn't trigger
		// "already is X" or "always fail" warnings from type narrowing
		for (UEdGraphPin* Pin : CastNode->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				Pin->PinType.PinSubCategoryObject = UObject::StaticClass();
				break;
			}
		}

		if (Expr->Children.Num() > 0 && Expr->Children[0])
		{
			int32 SubX = X - 200;
			int32 SubY = Y;
			UEdGraphPin* InputObj = ResolveDataExpr(Graph, Expr->Children[0], SubX, SubY);
			for (UEdGraphPin* Pin : CastNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					ForceConnect(InputObj, Pin);
					break;
				}
			}
		}

		X -= NodeSpacingX / 2;

		// Return the "As X" output pin
		for (UEdGraphPin* Pin : CastNode->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	// PRIMITIVE CAST (type conversions: InterfaceToBool, ObjectToBool, etc.)
	case EX_PrimitiveCast:
	{
		// ByteValue holds the ECastToken conversion type.
		// Children[0] is the value being converted.
		if (Expr->Children.Num() == 0 || !Expr->Children[0]) return nullptr;

		uint8 ConversionType = Expr->ByteValue;
		TSharedPtr<FDecompiledExpr> ChildExpr = Expr->Children[0];

		// For ObjectToBool (0x47) and InterfaceToBool (0x49):
		// The child is typically a DynamicCast result variable. The boolean
		// "success" is already on the DynamicCast node as an output pin.
		// Resolve the child to find the cast node, then return its boolean pin.
		const uint8 CST_ObjectToBool = 0x47;
		const uint8 CST_InterfaceToBool = 0x49;

		if (ConversionType == CST_ObjectToBool || ConversionType == CST_InterfaceToBool)
		{
			int32 SubX = X;
			int32 SubY = Y;
			UEdGraphPin* ChildPin = ResolveDataExpr(Graph, ChildExpr, SubX, SubY);
			if (ChildPin)
			{
				UEdGraphNode* OwnerNode = ChildPin->GetOwningNode();
				if (OwnerNode)
				{
					// Find the boolean output pin on the cast node
					for (UEdGraphPin* Pin : OwnerNode->Pins)
					{
						if (Pin->Direction == EGPD_Output &&
							Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
						{
							return Pin;
						}
					}
				}
			}
			return nullptr;
		}

		// For other conversion types: just resolve the child directly.
		// The conversion is implicit in Blueprint graphs.
		int32 SubX = X;
		int32 SubY = Y;
		return ResolveDataExpr(Graph, ChildExpr, SubX, SubY);
	}

	// SKIP OFFSET CONSTANT (used in LatentActionInfo struct construction)
	case EX_SkipOffsetConst:
	{
		// SkipOffsetConst provides a bytecode offset (typically for latent
		// action resume points). In the graph this is just an integer literal.
		// Return nullptr — the consuming pin will use its default value,
		// which is correct for LatentActionInfo Linkage fields that are
		// auto-populated by the compiler at cook time.
		return nullptr;
	}

	// CONTEXT (data access on another object)
	case EX_Context:
	case EX_Context_FailSilent:
	{
		if (Expr->Children.Num() > 0 && Expr->Children[0])
		{
			TSharedPtr<FDecompiledExpr> InnerExpr = Expr->Children[0];

			// Chain nested contexts (same as CreateNodeForExpr)
			// Safety: avoid creating cycles when expression nodes are shared.
			if (InnerExpr->ContextObject && Expr->ContextObject
				&& InnerExpr->ContextObject.Get() != Expr->ContextObject.Get())
			{
				bool bWouldCycle = false;
				for (TSharedPtr<FDecompiledExpr> Walk = Expr->ContextObject; Walk; Walk = Walk->ContextObject)
				{
					if (Walk.Get() == InnerExpr->ContextObject.Get())
					{
						bWouldCycle = true;
						break;
					}
				}
				if (!bWouldCycle)
				{
					InnerExpr->ContextObject->ContextObject = Expr->ContextObject;
				}
			}
			else if (!InnerExpr->ContextObject)
			{
				InnerExpr->ContextObject = Expr->ContextObject;
			}

			// Propagate FieldRef for type resolution (same as exec handler)
			if (InnerExpr->ContextObject && Expr->FieldRef)
			{
				if (!InnerExpr->ContextObject->FieldRef)
				{
					InnerExpr->ContextObject->FieldRef = Expr->FieldRef;
				}
			}

			Expr = InnerExpr; // Iterate instead of recursing (tail-call elimination)
			continue;
		}
		return nullptr;
	}

	// STRUCT MEMBER ACCESS
	// ARRAY GET BY REFERENCE → create UK2Node_GetArrayItem node
	case EX_ArrayGetByRef:
	{
		// EX_ArrayGetByRef: Children[0] = array expression, Children[1] = index expression
		// Used for struct arrays to get an element by reference (avoids copy).
		// In UE4.27 this maps to UK2Node_GetArrayItem (the modern Array GET node),
		// NOT the deprecated UK2Node_CallArrayFunction(Array_Get) which gets
		// auto-migrated and can break pin connections.
		if (Expr->Children.Num() < 2 || !Expr->Children[0] || !Expr->Children[1])
		{
			return nullptr;
		}

		// Resolve the array expression first so we can determine element type
		int32 ArrX = X - 200;
		int32 ArrY = Y + DataNodeOffsetY;
		UEdGraphPin* ArrPin = ResolveDataExpr(Graph, Expr->Children[0], ArrX, ArrY);
		if (!ArrPin)
		{
			Warn(TEXT("EX_ArrayGetByRef: Could not resolve array expression"));
			return nullptr;
		}

		// Determine the array element type from the resolved array pin
		FEdGraphPinType ElemType;
		bool bHasElemType = false;
		if (ArrPin->PinType.ContainerType == EPinContainerType::Array)
		{
			ElemType = ArrPin->PinType;
			ElemType.ContainerType = EPinContainerType::None;
			ElemType.bIsReference = false;
			bHasElemType = true;
		}
		else if (Expr->Children[0]->PropertyRef)
		{
			// Try from the property metadata
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Expr->Children[0]->PropertyRef);
			if (ArrayProp && ArrayProp->Inner)
			{
				GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(ArrayProp->Inner, ElemType);
				bHasElemType = true;
			}
		}

		// Create UK2Node_GetArrayItem — the proper UE4.27 array GET node
		// Pins: [0] "Array" (input), [1] "Dimension 1" (index input), [2] "Output" (result)
		UK2Node_GetArrayItem* GetNode = NewObject<UK2Node_GetArrayItem>(Graph);
		GetNode->CreateNewGuid();
		GetNode->NodePosX = X;
		GetNode->NodePosY = Y;
		GetNode->AllocateDefaultPins();
		Graph->AddNode(GetNode, false, false);

		// If we know the element type, propagate it to the Array and Output pins
		// (mirrors UK2Node_GetArrayItem::PropagatePinType which is private)
		if (bHasElemType)
		{
			UEdGraphPin* ArrayPin = GetNode->GetTargetArrayPin();
			if (ArrayPin)
			{
				ArrayPin->PinType = ElemType;
				ArrayPin->PinType.ContainerType = EPinContainerType::Array;
				ArrayPin->PinType.bIsReference = false;
			}
			UEdGraphPin* ResultPin = GetNode->GetResultPin();
			if (ResultPin)
			{
				ResultPin->PinType = ElemType;
				ResultPin->PinType.ContainerType = EPinContainerType::None;
				ResultPin->PinType.bIsConst = false;
			}
		}

		// Wire array input
		UEdGraphPin* TargetArrayPin = GetNode->GetTargetArrayPin();
		if (ArrPin && TargetArrayPin)
		{
			ForceConnect(ArrPin, TargetArrayPin);
		}

		// Wire index input
		int32 IdxX = X - 200;
		int32 IdxY = Y + DataNodeOffsetY * 2;
		UEdGraphPin* IdxPin = ResolveDataExpr(Graph, Expr->Children[1], IdxX, IdxY);
		UEdGraphPin* IndexPin = GetNode->GetIndexPin();
		if (IdxPin && IndexPin)
		{
			ForceConnect(IdxPin, IndexPin);
		}

		// Return the result output pin
		UEdGraphPin* OutputPin = GetNode->GetResultPin();
		X -= NodeSpacingX;
		return OutputPin;
	}

	case EX_StructMemberContext:
	{
		FProperty* MemberProp = Expr->PropertyRef;
		if (!MemberProp || Expr->Children.Num() == 0 || !Expr->Children[0])
		{
			return nullptr;
		}

		// Get the struct type that contains this member
		UScriptStruct* StructType = Cast<UScriptStruct>(MemberProp->GetOwnerStruct());
		if (!StructType)
		{
			// Can't determine struct type — fall through to resolve child directly
			Expr = Expr->Children[0];
			continue;
		}

		// Resolve the struct source expression to get the struct output pin.
		// This recursion handles nested struct access naturally:
		// e.g., A.B.C -> BreakStruct(A)->B pin -> BreakStruct(B)->C pin
		int32 BreakX = X - NodeSpacingX;
		int32 BreakY = Y;
		UEdGraphPin* StructPin = ResolveDataExpr(Graph, Expr->Children[0], BreakX, BreakY);
		if (!StructPin)
		{
			Warn(FString::Printf(TEXT("Break Struct: could not resolve struct source for member '%s' on %s"),
				*MemberProp->GetName(), *StructType->GetName()));
			return nullptr;
		}

		// Check if this struct has a native break function (e.g., FVector, FRotator).
		// Native break structs use specialized function calls in bytecode, not
		// EX_StructMemberContext, so this should be rare. But check to be safe.
		// UK2Node_BreakStruct::CanBeBroken is MinimalAPI and not exported, so
		// inline the essential check: "HasNativeBreak" metadata.
		if (StructType->HasMetaData(TEXT("HasNativeBreak")))
		{
			// Struct has a native break function — return struct pin as-is.
			Warn(FString::Printf(TEXT("Break Struct: %s has native break, returning struct pin"),
				*StructType->GetName()));
			return StructPin;
		}

		// Create a Break Struct node
		UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Graph);
		BreakNode->StructType = StructType;
		BreakNode->bMadeAfterOverridePinRemoval = true;
		BreakNode->NodePosX = X;
		BreakNode->NodePosY = Y;
		BreakNode->AllocateDefaultPins();
		Graph->AddNode(BreakNode, false, false);

		// Connect the struct source -> Break Struct input pin
		UEdGraphPin* InputPin = nullptr;
		for (UEdGraphPin* Pin : BreakNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				InputPin = Pin;
				break;
			}
		}
		if (InputPin)
		{
			TryConnect(StructPin, InputPin);
		}

		// Find the output pin matching the member property name
		FName MemberName = MemberProp->GetFName();
		for (UEdGraphPin* Pin : BreakNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinName == MemberName)
			{
				return Pin;
			}
		}

		// Pin name might not match exactly — try case-insensitive
		for (UEdGraphPin* Pin : BreakNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output &&
				Pin->PinName.ToString().Equals(MemberName.ToString(), ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}

		Warn(FString::Printf(TEXT("Break Struct: could not find output pin '%s' on %s (has %d pins)"),
			*MemberName.ToString(), *StructType->GetName(), BreakNode->Pins.Num()));

		// Fallback: return first output pin if available
		for (UEdGraphPin* Pin : BreakNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	// VIRTUAL FUNCTION CALLS AS DATA
	case EX_VirtualFunction:
	case EX_LocalVirtualFunction:
	{
		// Try to resolve and delegate to CallFunction handling
		UFunction* ResolvedFunc = nullptr;
		FName FuncName = Expr->NameValue;

		// If there's a context object, resolve from context type FIRST
		// (same priority fix as in the exec statement handler)
		if (Expr->ContextObject)
		{
			TSharedPtr<FDecompiledExpr> CtxExpr = Expr->ContextObject;
			while (CtxExpr && !ResolvedFunc)
			{
				// Check FieldRef first (propagated from EX_Context R-value property).
				// This carries the specific type info even when the expression itself
				// has already been unwrapped from EX_Context (e.g., a virtual function
				// call like GetPlayerHUD with FieldRef = PlayerHUD_C property).
				// NOTE: Do NOT restrict this to EX_Context tokens — the EX_Context
				// handler unwraps and propagates FieldRef to the inner expression,
				// so by this point the token is the actual expression type.
				if (CtxExpr->FieldRef)
				{
					FProperty* Prop = CastField<FProperty>(CtxExpr->FieldRef);
					if (Prop)
					{
						FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
						if (ObjProp && ObjProp->PropertyClass)
							ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
						if (!ResolvedFunc)
						{
							FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(Prop);
							if (IntProp && IntProp->InterfaceClass)
								ResolvedFunc = IntProp->InterfaceClass->FindFunctionByName(FuncName);
						}
					}
				}
				// Check if context is an EX_InterfaceContext wrapping a variable
				if (!ResolvedFunc && CtxExpr->Token == EX_InterfaceContext && CtxExpr->Children.Num() > 0)
				{
					TSharedPtr<FDecompiledExpr> InnerExpr = CtxExpr->Children[0];
					if (InnerExpr && InnerExpr->PropertyRef)
					{
						FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(InnerExpr->PropertyRef);
						if (IntProp && IntProp->InterfaceClass)
							ResolvedFunc = IntProp->InterfaceClass->FindFunctionByName(FuncName);
						if (!ResolvedFunc)
						{
							FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(InnerExpr->PropertyRef);
							if (ObjProp && ObjProp->PropertyClass)
								ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
						}
					}
				}
				// Check variable's PropertyRef (direct type from FProperty)
				if (!ResolvedFunc && CtxExpr->PropertyRef)
				{
					FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(CtxExpr->PropertyRef);
					if (ObjProp && ObjProp->PropertyClass)
						ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
					if (!ResolvedFunc)
					{
						FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(CtxExpr->PropertyRef);
						if (IntProp && IntProp->InterfaceClass)
							ResolvedFunc = IntProp->InterfaceClass->FindFunctionByName(FuncName);
					}
				}
				// Check if context is a function call result — infer from return type
				if (!ResolvedFunc && CtxExpr->FunctionRef)
				{
					FProperty* RetProp = CtxExpr->FunctionRef->GetReturnProperty();
					if (RetProp)
					{
						FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(RetProp);
						if (ObjProp && ObjProp->PropertyClass)
							ResolvedFunc = ObjProp->PropertyClass->FindFunctionByName(FuncName);
					}
				}
				// Check if context is an object constant (e.g., CDO Default__BFL_Post_C)
				if (!ResolvedFunc && CtxExpr->ObjectRef)
				{
					UClass* ObjClass = CtxExpr->ObjectRef->GetClass();
					if (ObjClass)
						ResolvedFunc = ObjClass->FindFunctionByName(FuncName);
				}
				if (CtxExpr->ContextObject)
					CtxExpr = CtxExpr->ContextObject;
				else if (CtxExpr->Children.Num() > 0)
					CtxExpr = CtxExpr->Children[0];
				else
					break;
			}
		}

		// Fall back to self-class resolution
		if (!ResolvedFunc && CurrentOriginalClass)
		{
			ResolvedFunc = CurrentOriginalClass->FindFunctionByName(FuncName);
		}
		if (!ResolvedFunc && Graph->GetOuter())
		{
			UBlueprint* BP = Cast<UBlueprint>(Graph->GetOuter());
			if (BP && BP->ParentClass)
			{
				ResolvedFunc = BP->ParentClass->FindFunctionByName(FuncName);
			}
		}
		if (!ResolvedFunc)
		{
			ResolvedFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FuncName);
		}
		if (!ResolvedFunc)
		{
			ResolvedFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(FuncName);
		}

		if (ResolvedFunc)
		{
			Expr->FunctionRef = ResolvedFunc;
			Expr->Token = EX_FinalFunction; // Must change token so re-dispatch hits FinalFunction case
			continue; // Iterate instead of recursing (tail-call elimination)
		}

		Warn(FString::Printf(TEXT("Could not resolve virtual function %s for data expression"), *Expr->NameValue.ToString()));
		return nullptr;
	}

	// STRUCT CONSTANT (e.g., FTransform literal in bytecode)
	case EX_StructConst:
	{
		if (!Expr->StructRef) return nullptr;

		FString StructName = Expr->StructRef->GetName();

		// FLatentActionInfo: The BP compiler auto-populates these for latent actions
		// (Delay, MoveComponentTo, etc.). Never create nodes for them.
		if (StructName == TEXT("LatentActionInfo"))
		{
			return nullptr;
		}

		// For FTransform, FVector, FRotator, FLinearColor, etc. use
		// UKismetMathLibrary::Make* functions which are always available.
		FName MakeFuncName = NAME_None;
		UClass* MakeLibrary = UKismetMathLibrary::StaticClass();

		if (StructName == TEXT("Transform"))
		{
			MakeFuncName = TEXT("MakeTransform");
		}
		else if (StructName == TEXT("Vector"))
		{
			MakeFuncName = TEXT("MakeVector");
		}
		else if (StructName == TEXT("Rotator"))
		{
			MakeFuncName = TEXT("MakeRotator");
		}
		else if (StructName == TEXT("Vector2D"))
		{
			MakeFuncName = TEXT("MakeVector2D");
		}
		else if (StructName == TEXT("LinearColor"))
		{
			MakeFuncName = TEXT("MakeColor");
		}

		if (MakeFuncName != NAME_None)
		{
			UFunction* MakeFunc = MakeLibrary->FindFunctionByName(MakeFuncName);
			if (MakeFunc)
			{
				UK2Node_CallFunction* MakeNode = NewObject<UK2Node_CallFunction>(Graph);
				MakeNode->CreateNewGuid();
				MakeNode->SetFromFunction(MakeFunc);
				MakeNode->NodePosX = X;
				MakeNode->NodePosY = Y;
				MakeNode->AllocateDefaultPins();
				Graph->AddNode(MakeNode, false, false);

				// Set member literal defaults from children
				int32 ChildIdx = 0;
				int32 ParamIdx = 0;
				for (TFieldIterator<FProperty> It(MakeFunc); It; ++It)
				{
					FProperty* Param = *It;
					if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
						continue;

					if (ChildIdx < Expr->Children.Num() && Expr->Children[ChildIdx])
					{
						TSharedPtr<FDecompiledExpr> ChildExpr = Expr->Children[ChildIdx];
						UEdGraphPin* ParamPin = FindDataPin(MakeNode, EGPD_Input, Param->GetFName());
						if (ParamPin)
						{
							if (ChildExpr->IsLiteral())
							{
								// Check if pin expects an enum — resolve name instead of raw int
								bool bEnumResolved = false;
								if ((ChildExpr->Token == EX_ByteConst || ChildExpr->Token == EX_IntConst ||
									ChildExpr->Token == EX_IntConstByte || ChildExpr->Token == EX_IntZero || ChildExpr->Token == EX_IntOne) &&
									ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
									ParamPin->PinType.PinSubCategoryObject.IsValid())
								{
									UEnum* PinEnum = Cast<UEnum>(ParamPin->PinType.PinSubCategoryObject.Get());
									if (PinEnum)
									{
										int64 EnumVal = (ChildExpr->Token == EX_ByteConst) ? ChildExpr->ByteValue : ChildExpr->IntValue;
										FString EnumName = PinEnum->GetNameStringByValue(EnumVal);
										if (!EnumName.IsEmpty())
										{
											ParamPin->DefaultValue = EnumName;
											bEnumResolved = true;
										}
									}
								}
								if (!bEnumResolved)
								{
									FString Val = ChildExpr->GetLiteralAsString();
									if (!Val.IsEmpty())
									{
										ParamPin->DefaultValue = Val;
									}
								}
							}
							else
							{
								// Recursively resolve (handles nested struct constants)
								int32 SubX = X - 250;
								int32 SubY = Y + ChildIdx * 50;
								UEdGraphPin* ChildPin = ResolveDataExpr(Graph, ChildExpr, SubX, SubY);
								ForceConnect(ChildPin, ParamPin);
							}
						}
					}
					ChildIdx++;
					ParamIdx++;
				}

				X -= NodeSpacingX;

				// Return the ReturnValue output pin
				UEdGraphPin* RetPin = FindDataPin(MakeNode, EGPD_Output, UEdGraphSchema_K2::PN_ReturnValue);
				return RetPin ? RetPin : FindFirstDataOutputPin(MakeNode);
			}
		}

		// Unknown struct type — return nullptr (pin will use default value)
		return nullptr;
	}

	// ARRAY CONSTANT → K2Node_MakeArray
	case EX_ArrayConst:
	{
		int32 NumElements = Expr->IntValue;

		UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Graph);
		MakeArrayNode->CreateNewGuid();
		MakeArrayNode->NumInputs = FMath::Max(NumElements, 1);
		MakeArrayNode->NodePosX = X;
		MakeArrayNode->NodePosY = Y;
		MakeArrayNode->AllocateDefaultPins();
		Graph->AddNode(MakeArrayNode, false, false);

		// Set element type from the inner property
		if (Expr->PropertyRef)
		{
			FEdGraphPinType ElemPinType;
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Expr->PropertyRef, ElemPinType);

			for (UEdGraphPin* Pin : MakeArrayNode->Pins)
			{
				if (Pin->Direction == EGPD_Output)
				{
					Pin->PinType = ElemPinType;
					Pin->PinType.ContainerType = EPinContainerType::Array;
				}
				else if (Pin->Direction == EGPD_Input &&
					Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					Pin->PinType = ElemPinType;
				}
			}
		}

		// Wire element values
		int32 ElemX = X - 250;
		int32 ElemY = Y;
		for (int32 i = 0; i < NumElements && i < Expr->Children.Num(); i++)
		{
			TSharedPtr<FDecompiledExpr> ElemExpr = Expr->Children[i];
			if (!ElemExpr) continue;

			UEdGraphPin* ElemPin = nullptr;
			int32 DataIdx = 0;
			for (UEdGraphPin* Pin : MakeArrayNode->Pins)
			{
				if (Pin->Direction == EGPD_Input &&
					Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					if (DataIdx == i) { ElemPin = Pin; break; }
					DataIdx++;
				}
			}

			if (ElemPin)
			{
				// Handle EX_Self and EX_NoObject specially (same as EX_SetArray handler)
				if (ElemExpr->Token == EX_Self)
				{
					UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
					SelfNode->CreateNewGuid();
					SelfNode->NodePosX = ElemX;
					SelfNode->NodePosY = ElemY;
					SelfNode->AllocateDefaultPins();
					Graph->AddNode(SelfNode, false, false);
					UEdGraphPin* SelfOut = FindFirstDataOutputPin(SelfNode);
					ForceConnect(SelfOut, ElemPin);
					ElemY += NodeSpacingY / 3;
				}
				else if (ElemExpr->Token == EX_NoObject)
				{
					ElemPin->DefaultValue = TEXT("");
				}
				else if (ElemExpr->IsLiteral())
				{
					if ((ElemExpr->Token == EX_ByteConst || ElemExpr->Token == EX_IntConst ||
						ElemExpr->Token == EX_IntConstByte) &&
						ElemPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
						ElemPin->PinType.PinSubCategoryObject.IsValid())
					{
						UEnum* PinEnum = Cast<UEnum>(ElemPin->PinType.PinSubCategoryObject.Get());
						if (PinEnum)
						{
							int64 Val = (ElemExpr->Token == EX_ByteConst) ? ElemExpr->ByteValue : ElemExpr->IntValue;
							FString EnumName = PinEnum->GetNameStringByValue(Val);
							ElemPin->DefaultValue = EnumName.IsEmpty() ? ElemExpr->GetLiteralAsString() : EnumName;
						}
						else
						{
							ElemPin->DefaultValue = ElemExpr->GetLiteralAsString();
						}
					}
					else if (ElemExpr->Token == EX_ObjectConst)
					{
						if (ElemExpr->ObjectRef)
						{
							ElemPin->DefaultObject = ElemExpr->ObjectRef;
						}
					}
					else
					{
						ElemPin->DefaultValue = ElemExpr->GetLiteralAsString();
					}
				}
				else
				{
					UEdGraphPin* ElemOut = ResolveDataExpr(Graph, ElemExpr, ElemX, ElemY);
					ForceConnect(ElemOut, ElemPin);
					ElemY += NodeSpacingY / 3;
				}
			}
		}

		X -= NodeSpacingX;
		return FindFirstDataOutputPin(MakeArrayNode);
	}

	// INTERFACE CONTEXT (wraps an interface-typed expression for context access)
	case EX_InterfaceContext:
	{
		// EX_InterfaceContext wraps a child expression — just resolve the child
		if (Expr->Children.Num() > 0 && Expr->Children[0])
		{
			Expr = Expr->Children[0]; // Iterate instead of recursing (tail-call elimination)
			continue;
		}
		return nullptr;
	}

	// SWITCH VALUE → UK2Node_Select
	case EX_SwitchValue:
	{
		// Children layout:
		//   [0] = index expression (the selector)
		//   [1 + i*2] = case i value (literal)
		//   [2 + i*2] = case i result (expression)
		//   [1 + NumCases*2] = default result expression
		int32 NumCases = Expr->IntValue;
		int32 ExpectedChildren = 1 + NumCases * 2 + 1;
		if (Expr->Children.Num() < ExpectedChildren)
		{
			Warn(FString::Printf(TEXT("SwitchValue: expected %d children, got %d"),
				ExpectedChildren, Expr->Children.Num()));
			return nullptr;
		}

		TSharedPtr<FDecompiledExpr> IndexExpr = Expr->Children[0];

		// Determine value type from case result expressions.
		// Strategy: scan case results for type info using multiple methods:
		//   1) PropertyRef on the expression (direct property access)
		//   2) Literal token type (EX_NameConst→PC_Name, EX_StringConst→PC_String, etc.)
		//   3) TempLiteralMap lookup for temp variables that hold literals
		//   4) Fallback: use IndexPinType for enum-indexed Selects
		FEdGraphPinType ValuePinType;
		ValuePinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		{
			// Helper lambda: infer pin type from a literal expression's token
			auto InferTypeFromLiteralToken = [](EExprToken Token, TSharedPtr<FDecompiledExpr> LitExpr, FEdGraphPinType& OutType) -> bool
			{
				switch (Token)
				{
				case EX_NameConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
					return true;
				case EX_StringConst:
				case EX_UnicodeStringConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_String;
					return true;
				case EX_IntConst:
				case EX_IntConstByte:
				case EX_IntZero:
				case EX_IntOne:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
					return true;
				case EX_Int64Const:
				case EX_UInt64Const:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
					return true;
				case EX_FloatConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Float;
					return true;
				case EX_True:
				case EX_False:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
					return true;
				case EX_ByteConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					return true;
				case EX_TextConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
					return true;
				case EX_ObjectConst:
				case EX_NoObject:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
					if (LitExpr && LitExpr->ObjectRef)
					{
						OutType.PinSubCategoryObject = LitExpr->ObjectRef->GetClass();
					}
					else
					{
						OutType.PinSubCategoryObject = UObject::StaticClass();
					}
					return true;
				case EX_VectorConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					OutType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
					return true;
				case EX_RotationConst:
					OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					OutType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
					return true;
				default:
					return false;
				}
			};

			// Scan ALL case results to find type info, then check for compatibility.
			// If cases have different object types (e.g., Pawn vs MasterUI_C), widen
			// to UObject so both can wire without type-mismatch errors.
			TArray<FEdGraphPinType> CaseResultTypes;
			for (int32 ci = 0; ci < NumCases; ci++)
			{
				TSharedPtr<FDecompiledExpr> CaseResult = Expr->Children[2 + ci * 2];
				if (!CaseResult) continue;

				FEdGraphPinType CaseType;
				CaseType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;

				// Method 1: PropertyRef on the expression
				if (CaseResult->PropertyRef)
				{
					GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(CaseResult->PropertyRef, CaseType);
				}
				// Method 2: Literal token type
				else if (CaseResult->IsLiteral())
				{
					InferTypeFromLiteralToken(CaseResult->Token, CaseResult, CaseType);
				}
				// Method 3: Temp variable → look up underlying literal or property type
				else if (CaseResult->Token == EX_LocalVariable && CaseResult->PropertyRef)
				{
					FName VarName = CaseResult->PropertyRef->GetFName();
					TSharedPtr<FDecompiledExpr>* StoredLit = TempLiteralMap.Find(VarName);
					if (StoredLit && (*StoredLit)->IsLiteral())
					{
						InferTypeFromLiteralToken((*StoredLit)->Token, *StoredLit, CaseType);
					}
					if (CaseType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					{
						GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(CaseResult->PropertyRef, CaseType);
					}
				}

				if (CaseType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
				{
					CaseResultTypes.Add(CaseType);
				}
			}

			// Determine common type from all case results
			if (CaseResultTypes.Num() > 0)
			{
				ValuePinType = CaseResultTypes[0];

				// Check if all resolved types match
				bool bAllSame = true;
				for (int32 i = 1; i < CaseResultTypes.Num(); i++)
				{
					if (CaseResultTypes[i].PinCategory != ValuePinType.PinCategory ||
						CaseResultTypes[i].PinSubCategoryObject != ValuePinType.PinSubCategoryObject)
					{
						bAllSame = false;
						break;
					}
				}

				// If types differ and are all object types, widen to UObject
				if (!bAllSame)
				{
					bool bAllObjects = true;
					for (const FEdGraphPinType& CaseType : CaseResultTypes)
					{
						if (CaseType.PinCategory != UEdGraphSchema_K2::PC_Object &&
							CaseType.PinCategory != UEdGraphSchema_K2::PC_Interface &&
							CaseType.PinCategory != UEdGraphSchema_K2::PC_SoftObject)
						{
							bAllObjects = false;
							break;
						}
					}

					if (bAllObjects)
					{
						ValuePinType.PinCategory = UEdGraphSchema_K2::PC_Object;
						ValuePinType.PinSubCategoryObject = UObject::StaticClass();
					}
				}
			}
		}

		// Determine index pin type
		FEdGraphPinType IndexPinType;
		IndexPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		if (IndexExpr && IndexExpr->PropertyRef)
		{
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(IndexExpr->PropertyRef, IndexPinType);
		}

		// Fallback: if ValuePinType is still WILDCARD and index is an enum type,
		// use the enum type as the value type (enum-indexed Select returning enum values)
		if (ValuePinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard &&
			IndexPinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
			IndexPinType.PinSubCategoryObject.IsValid())
		{
			ValuePinType = IndexPinType;
		}

		// Create UK2Node_Select.
		// CRITICAL: UK2Node_Select has a private NumOptionPins member that
		// AllocateDefaultPins reads on reconstruction. If we add pins manually
		// without updating NumOptionPins, on save→reload the node reconstructs
		// with only 2 option pins, losing extras and breaking bool→int conversion.
		//
		// Solution: Set NumOptionPins via UE4 property reflection BEFORE
		// calling AllocateDefaultPins so it creates the right number of pins.
		UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Graph);
		SelectNode->CreateNewGuid();
		SelectNode->NodePosX = X;
		SelectNode->NodePosY = Y;

		// Set NumOptionPins via reflection before AllocateDefaultPins
		if (NumCases > 2)
		{
			FIntProperty* NumOptionPinsProp = CastField<FIntProperty>(
				UK2Node_Select::StaticClass()->FindPropertyByName(TEXT("NumOptionPins")));
			if (NumOptionPinsProp)
			{
				NumOptionPinsProp->SetPropertyValue_InContainer(SelectNode, NumCases);
			}
			else
			{
				Warn(TEXT("Could not find NumOptionPins property on UK2Node_Select — extra option pins may not survive save/reload"));
			}
		}

		// Set IndexPinType member via reflection BEFORE AllocateDefaultPins.
		// UK2Node_Select has a serialized IndexPinType UPROPERTY that controls
		// how it reconstructs on reload. If this isn't set, the node loses
		// its bool index type on save→reload, and the BoolToInt expansion fails.
		//
		// CRITICAL: Do NOT set PinSubCategoryObject (the enum) on the member.
		// UK2Node_Select::GetOptionPins() checks IndexPinType for an enum
		// SubCategoryObject. If found, it looks for pins named after EnumEntries.
		// But our pins are named "Option 0", "Option 1", etc. — not enum names.
		// Since we never call SetEnum(), EnumEntries is empty, so GetOptionPins()
		// returns ZERO pins. This causes:
		//   - Post-pass type propagation to find no option pins
		//   - Reconstruction to skip option pin type filling
		//   - Compiler's "No option pin in Select" errors
		// Fix: set the member to just PC_Byte (or PC_Boolean/PC_Int) without
		// the enum SubCategoryObject. The actual Index PIN still gets the full
		// enum type for proper wire connections.
		if (IndexPinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			FStructProperty* IndexPinTypeProp = CastField<FStructProperty>(
				UK2Node_Select::StaticClass()->FindPropertyByName(TEXT("IndexPinType")));
			if (IndexPinTypeProp)
			{
				FEdGraphPinType* MemberPtr = IndexPinTypeProp->ContainerPtrToValuePtr<FEdGraphPinType>(SelectNode);
				if (MemberPtr)
				{
					// Copy the type but strip enum SubCategoryObject so
					// GetOptionPins() uses the generic "Option N" path
					FEdGraphPinType MemberType = IndexPinType;
					if (MemberType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
						MemberType.PinSubCategoryObject.IsValid() &&
						MemberType.PinSubCategoryObject->IsA(UEnum::StaticClass()))
					{
						// Keep as PC_Byte but without the enum — this makes
						// GetOptionPins() use the "Option N" name matching
						// instead of the EnumEntries name matching
						MemberType.PinSubCategoryObject = nullptr;
					}
					*MemberPtr = MemberType;
				}
			}
		}

		SelectNode->AllocateDefaultPins();
		Graph->AddNode(SelectNode, false, false);

		// Set index pin type BEFORE wiring so the node knows its index type.
		// For bool-indexed Select nodes, this is critical — UK2Node_Select
		// generates a BoolToInt conversion during ExpandNode which requires
		// the index pin type to be properly set.
		UEdGraphPin* IndexPin = SelectNode->GetIndexPin();
		if (IndexPin && IndexPinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			IndexPin->PinType = IndexPinType;
		}

		// Force return value pin type
		UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
		if (ReturnPin && ValuePinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			ReturnPin->PinType = ValuePinType;
		}

		// Wire the index expression
		if (IndexPin)
		{
			int32 IdxX = X - NodeSpacingX / 2;
			int32 IdxY = Y - NodeSpacingY;
			UEdGraphPin* IndexOut = ResolveDataExpr(Graph, IndexExpr, IdxX, IdxY);
			if (IndexOut)
			{
				ForceConnect(IndexOut, IndexPin);
			}
			else
			{
				// Fallback: if the index is a Temp_ variable holding a literal
				// (e.g., Temp_bool_Variable set to EX_True/EX_False), apply it as
				// a pin default. Without this, the index pin keeps its default
				// value (false), breaking bool-indexed Selects that should be true.
				ApplyTempLiteralToPin(IndexExpr, IndexPin);
			}
		}

		// Wire each case result to its option pin
		for (int32 i = 0; i < NumCases; i++)
		{
			TSharedPtr<FDecompiledExpr> CaseResult = Expr->Children[2 + i * 2];
			UEdGraphPin* OptionPin = SelectNode->FindPin(FName(*FString::Printf(TEXT("Option %d"), i)));

			if (OptionPin && CaseResult)
			{
				// Force option pin type to match value type
				if (ValuePinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
				{
					OptionPin->PinType = ValuePinType;
				}

				// Try to set as literal default first (most common for SwitchValue)
				if (ApplyTempLiteralToPin(CaseResult, OptionPin))
				{
					// Literal applied
				}
				else if (CaseResult->IsLiteral())
				{
					// Check if pin expects an enum — resolve name instead of raw int
					bool bEnumResolved = false;
					if ((CaseResult->Token == EX_ByteConst || CaseResult->Token == EX_IntConst ||
						CaseResult->Token == EX_IntConstByte || CaseResult->Token == EX_IntZero || CaseResult->Token == EX_IntOne) &&
						OptionPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte &&
						OptionPin->PinType.PinSubCategoryObject.IsValid())
					{
						UEnum* PinEnum = Cast<UEnum>(OptionPin->PinType.PinSubCategoryObject.Get());
						if (PinEnum)
						{
							int64 Val = (CaseResult->Token == EX_ByteConst) ? CaseResult->ByteValue : CaseResult->IntValue;
							FString EnumName = PinEnum->GetNameStringByValue(Val);
							if (!EnumName.IsEmpty())
							{
								OptionPin->DefaultValue = EnumName;
								bEnumResolved = true;
							}
						}
					}
					if (!bEnumResolved)
					{
						OptionPin->DefaultValue = CaseResult->GetLiteralAsString();
					}
				}
				else
				{
					// Wire from an expression
					int32 OptX = X - NodeSpacingX;
					int32 OptY = Y + i * (NodeSpacingY / 2);
					UEdGraphPin* ResultOut = ResolveDataExpr(Graph, CaseResult, OptX, OptY);
					if (ResultOut)
					{
						ForceConnect(ResultOut, OptionPin);
					}
				}
			}
		}

		X -= NodeSpacingX;

		// CRITICAL: UK2Node_Select::AllocateDefaultPins creates all option pins as
		// PC_Wildcard. We set types above, but UK2Node_Select sets bReconstructNode=true
		// in response to various operations (wiring, type changes). When reconstruction
		// fires, it recreates all pins as Wildcard, losing our types.
		// Fix: force bReconstructNode=false so our types persist.
		{
			FBoolProperty* ReconstructProp = CastField<FBoolProperty>(
				UK2Node_Select::StaticClass()->FindPropertyByName(TEXT("bReconstructNode")));
			if (ReconstructProp)
			{
				ReconstructProp->SetPropertyValue_InContainer(SelectNode, false);
			}
		}

		return ReturnPin;
	}

	default:
		// For unhandled data expressions, just return nullptr
		// The calling code will leave the pin unconnected
		return nullptr;
	}
	} // end for(;;) iterative loop
}

// Struct Pin Default Fixup

void FGraphBuilder::FixStructPinDefaults(UEdGraphNode* Node)
{
	if (!Node) return;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
		if (Pin->LinkedTo.Num() > 0) continue; // Already connected

		FString& DefaultVal = Pin->DefaultValue;
		if (DefaultVal.IsEmpty()) continue;

		// Fix named Rotator format: (Pitch=X,Yaw=Y,Roll=Z) → X,Y,Z
		if (DefaultVal.Contains(TEXT("Pitch=")) && DefaultVal.Contains(TEXT("Yaw=")) && DefaultVal.Contains(TEXT("Roll=")))
		{
			// FRotator::InitFromString only handles short-form P=/Y=/R=,
			// but ExportTextItem produces long-form Pitch=/Yaw=/Roll=
			float Pitch = 0, Yaw = 0, Roll = 0;
			FParse::Value(*DefaultVal, TEXT("Pitch="), Pitch);
			FParse::Value(*DefaultVal, TEXT("Yaw="), Yaw);
			FParse::Value(*DefaultVal, TEXT("Roll="), Roll);
			Pin->DefaultValue = FString::Printf(TEXT("%f,%f,%f"), Pitch, Yaw, Roll);
		}
		// Fix named Vector format: (X=V,Y=V,Z=V) → V,V,V
		else if (DefaultVal.StartsWith(TEXT("(")) && DefaultVal.Contains(TEXT("X=")) &&
			DefaultVal.Contains(TEXT("Y=")) && DefaultVal.Contains(TEXT("Z=")))
		{
			FVector Vec;
			if (Vec.InitFromString(DefaultVal))
			{
				Pin->DefaultValue = FString::Printf(TEXT("%f,%f,%f"), Vec.X, Vec.Y, Vec.Z);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE

