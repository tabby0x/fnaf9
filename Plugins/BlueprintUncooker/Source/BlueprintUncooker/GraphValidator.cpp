#include "GraphValidator.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Knot.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputAction.h"
#include "K2Node_TemporaryVariable.h"

#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"

DEFINE_LOG_CATEGORY_STATIC(LogGraphValidator, Log, All);

// Pin Type to Human-Readable String

FString FGraphValidator::PinTypeToString(const FEdGraphPinType& PinType)
{
	FString Result;

	// Container prefix
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		Result = TEXT("Array<");
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		Result = TEXT("Set<");
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		Result = TEXT("Map<");
	}

	// Core type
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		Result += TEXT("Exec");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		Result += TEXT("Bool");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			Result += TEXT("Byte");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		Result += TEXT("Int");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		Result += TEXT("Int64");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Float)
	{
		Result += TEXT("Float");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		Result += TEXT("Name");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		Result += TEXT("String");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		Result += TEXT("Text");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			Result += TEXT("Struct");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			Result += TEXT("Object");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			Result += TEXT("Interface");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += FString::Printf(TEXT("Class<%s>"), *PinType.PinSubCategoryObject->GetName());
		}
		else
		{
			Result += TEXT("Class");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		Result += TEXT("WILDCARD");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			Result += TEXT("Enum");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject)
	{
		Result += TEXT("SoftObject");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		Result += TEXT("SoftClass");
	}
	else
	{
		Result += PinType.PinCategory.ToString();
	}

	// Close container
	if (PinType.ContainerType == EPinContainerType::Array ||
		PinType.ContainerType == EPinContainerType::Set ||
		PinType.ContainerType == EPinContainerType::Map)
	{
		Result += TEXT(">");
	}

	// Reference flag
	if (PinType.bIsReference)
	{
		Result += TEXT("&");
	}

	return Result;
}

// Node to Short Identifier

FString FGraphValidator::GetNodeFunctionName(UEdGraphNode* Node)
{
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		UFunction* Func = CallNode->GetTargetFunction();
		if (Func)
		{
			return Func->GetName();
		}
		// Fall back to member name
		return CallNode->FunctionReference.GetMemberName().ToString();
	}
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		if (EventNode->EventReference.GetMemberName() != NAME_None)
		{
			return EventNode->EventReference.GetMemberName().ToString();
		}
		return EventNode->GetFunctionName().ToString();
	}
	if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
	{
		return CustomEvent->CustomFunctionName.ToString();
	}
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		if (CastNode->TargetType)
		{
			return FString::Printf(TEXT("CastTo_%s"), *CastNode->TargetType->GetName());
		}
	}
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
	{
		return FString::Printf(TEXT("Get_%s"), *VarGet->VariableReference.GetMemberName().ToString());
	}
	if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
	{
		return FString::Printf(TEXT("Set_%s"), *VarSet->VariableReference.GetMemberName().ToString());
	}
	return TEXT("");
}

FString FGraphValidator::NodeToShortString(UEdGraphNode* Node)
{
	if (!Node) return TEXT("(null)");

	FString ClassName = Node->GetClass()->GetName();

	// Strip the K2Node_ prefix for readability
	ClassName.RemoveFromStart(TEXT("K2Node_"));

	FString FuncName = GetNodeFunctionName(Node);
	if (!FuncName.IsEmpty())
	{
		return FString::Printf(TEXT("%s: %s"), *ClassName, *FuncName);
	}

	return ClassName;
}

// Pin Dump

void FGraphValidator::DumpPin(UEdGraphPin* Pin, FString& Out)
{
	if (!Pin) return;

	bool bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	bool bIsWildcard = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;

	// Direction prefix
	FString DirStr;
	if (bIsExec)
	{
		DirStr = (Pin->Direction == EGPD_Input) ? TEXT("  [ExecIn] ") : TEXT("  [ExecOut]");
	}
	else
	{
		DirStr = (Pin->Direction == EGPD_Input) ? TEXT("  [In]     ") : TEXT("  [Out]    ");
	}

	// Pin name and type
	FString TypeStr = PinTypeToString(Pin->PinType);
	FString PinLabel = FString::Printf(TEXT("%s %s [%s]"), *DirStr, *Pin->GetDisplayName().ToString(), *TypeStr);

	// Connection info
	if (Pin->LinkedTo.Num() > 0)
	{
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				FString ConnStr = FString::Printf(TEXT("%s → %s.%s"),
					*PinLabel,
					*NodeToShortString(LinkedPin->GetOwningNode()),
					*LinkedPin->GetDisplayName().ToString());

				// Flag wildcard connections
				if (bIsWildcard || LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					ConnStr += TEXT("  *** WILDCARD ***");
					WildcardConnections++;
				}

				Out += FString::Printf(TEXT("%s\n"), *ConnStr);
			}
		}
	}
	else
	{
		// No connection
		if (!Pin->DefaultValue.IsEmpty() || Pin->DefaultObject)
		{
			// Has default value
			FString DefaultStr;
			if (Pin->DefaultObject)
			{
				DefaultStr = Pin->DefaultObject->GetName();
			}
			else
			{
				DefaultStr = Pin->DefaultValue;
			}

			// Truncate long defaults
			if (DefaultStr.Len() > 60)
			{
				DefaultStr = DefaultStr.Left(57) + TEXT("...");
			}

			Out += FString::Printf(TEXT("%s = %s\n"), *PinLabel, *DefaultStr);
		}
		else if (Pin->Direction == EGPD_Input && !bIsExec)
		{
			// Disconnected input data pin — potentially problematic
			// Skip Self pins (these default to self implicitly)
			if (Pin->PinName != UEdGraphSchema_K2::PN_Self && !Pin->bHidden)
			{
				Out += FString::Printf(TEXT("%s  *** DISCONNECTED ***\n"), *PinLabel);
				DisconnectedInputs++;
			}
		}
		else if (Pin->Direction == EGPD_Output && !bIsExec)
		{
			// Unconnected output — not necessarily a problem, just note it
			Out += FString::Printf(TEXT("%s (unused)\n"), *PinLabel);
		}
		else if (bIsExec && Pin->Direction == EGPD_Output)
		{
			Out += FString::Printf(TEXT("%s (no exec)\n"), *PinLabel);
		}
		// Skip disconnected exec inputs (normal for event/entry nodes)
	}
}

// Node Dump

void FGraphValidator::DumpNode(UEdGraphNode* Node, int32 NodeIndex, FString& Out)
{
	if (!Node) return;

	TotalNodes++;

	FString NodeDesc = NodeToShortString(Node);
	Out += FString::Printf(TEXT("\n[Node %d] %s @ (%d, %d)\n"),
		NodeIndex, *NodeDesc, Node->NodePosX, Node->NodePosY);

	// Extra context for VariableGet nodes (helps debug ghost nodes)
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
	{
		FString VarRef = VarGet->VariableReference.GetMemberName().ToString();
		bool bSelf = VarGet->VariableReference.IsSelfContext();
		FString OwnerClass = TEXT("(none)");
		if (UClass* MemberParent = VarGet->VariableReference.GetMemberParentClass())
		{
			OwnerClass = MemberParent->GetName();
		}
		Out += FString::Printf(TEXT("  [VarRef] %s  SelfContext=%s  OwnerClass=%s\n"),
			*VarRef, bSelf ? TEXT("true") : TEXT("false"), *OwnerClass);
	}

	// Extra context for VariableSet nodes
	if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
	{
		FString VarRef = VarSet->VariableReference.GetMemberName().ToString();
		bool bSelf = VarSet->VariableReference.IsSelfContext();
		Out += FString::Printf(TEXT("  [VarRef] %s  SelfContext=%s\n"),
			*VarRef, bSelf ? TEXT("true") : TEXT("false"));
	}

	// Dump exec pins first, then data pins
	// Input exec
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
		{
			DumpPin(Pin, Out);
		}
	}
	// Output exec
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
		{
			DumpPin(Pin, Out);
		}
	}
	// Input data
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
		{
			DumpPin(Pin, Out);
		}
	}
	// Output data
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
		{
			DumpPin(Pin, Out);
		}
	}
}

// Graph Dump

void FGraphValidator::DumpGraph(UEdGraph* Graph, FString& Out)
{
	if (!Graph) return;

	Out += FString::Printf(TEXT("\n--- GRAPH: %s (%d nodes) ---\n"), *Graph->GetName(), Graph->Nodes.Num());

	for (int32 i = 0; i < Graph->Nodes.Num(); i++)
	{
		DumpNode(Graph->Nodes[i], i, Out);
	}
}

// Blueprint Dump

void FGraphValidator::DumpBlueprint(UBlueprint* BP)
{
	if (!BP) return;

	// Reset counters
	TotalNodes = 0;
	DisconnectedInputs = 0;
	WildcardConnections = 0;

	FString Out;

	Out += FString::Printf(TEXT("=== GRAPH DUMP: %s ===\n"), *BP->GetName());
	Out += FString::Printf(TEXT("Parent: %s\n"), BP->ParentClass ? *BP->ParentClass->GetName() : TEXT("None"));

	// Event graphs (ubergraph pages)
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		DumpGraph(Graph, Out);
	}

	// Function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		DumpGraph(Graph, Out);
	}

	// Summary
	Out += FString::Printf(TEXT("\n=== SUMMARY ===\n"));
	Out += FString::Printf(TEXT("Total nodes: %d\n"), TotalNodes);
	Out += FString::Printf(TEXT("Disconnected inputs: %d\n"), DisconnectedInputs);
	Out += FString::Printf(TEXT("Wildcard connections: %d\n"), WildcardConnections);

	// === Flagged Issues (easy scanning section) ===
	// Re-parse the dump to collect all *** flagged lines with their node context
	{
		Out += FString::Printf(TEXT("\n=== FLAGGED ISSUES ===\n"));

		TArray<FString> Lines;
		Out.ParseIntoArrayLines(Lines);

		FString CurrentGraph;
		FString CurrentNode;
		FString LastReportedGraph;
		int32 IssueCount = 0;
		FString IssuesSection;

		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("--- GRAPH:")))
			{
				CurrentGraph = Line;
			}
			else if (Line.StartsWith(TEXT("\n[Node")) || Line.StartsWith(TEXT("[Node")))
			{
				CurrentNode = Line.TrimStartAndEnd();
			}
			else if (Line.Contains(TEXT("***")))
			{
				// Print graph header if we haven't for this graph yet
				if (CurrentGraph != LastReportedGraph)
				{
					IssuesSection += FString::Printf(TEXT("\n%s\n"), *CurrentGraph);
					LastReportedGraph = CurrentGraph;
				}
				IssuesSection += FString::Printf(TEXT("  %s\n"), *CurrentNode);
				IssuesSection += FString::Printf(TEXT("    %s\n"), *Line.TrimStartAndEnd());
				IssueCount++;
			}
		}

		if (IssueCount == 0)
		{
			Out += TEXT("  (no issues found)\n");
		}
		else
		{
			Out += IssuesSection;
			Out += FString::Printf(TEXT("\nTotal flagged issues: %d\n"), IssueCount);
		}
	}

	Out += FString::Printf(TEXT("\n=== END DUMP ===\n"));

	FString& DumpStr = Out;

	UE_LOG(LogGraphValidator, Log, TEXT("Dump complete for %s: %d nodes, %d disconnected inputs, %d wildcard connections"),
		*BP->GetName(), TotalNodes, DisconnectedInputs, WildcardConnections);

	FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintUncooker"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	FString OutputPath = FPaths::Combine(OutputDir, FString::Printf(TEXT("%s_dump.txt"), *BP->GetName()));

	if (FFileHelper::SaveStringToFile(DumpStr, *OutputPath))
	{
		UE_LOG(LogGraphValidator, Log, TEXT("Full dump written to: %s"), *OutputPath);
	}
	else
	{
		UE_LOG(LogGraphValidator, Warning, TEXT("Failed to write dump to: %s"), *OutputPath);
		// Fall back to logging everything
		TArray<FString> Lines;
		DumpStr.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogGraphValidator, Log, TEXT("%s"), *Line);
		}
	}
}