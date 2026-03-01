#pragma once

#include "CoreMinimal.h"
#include "BytecodeReader.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Post-uncook graph validator and dumper.
 * 
 * Walks every node/pin in the uncooked Blueprint and outputs a structured
 * text report showing exactly what's connected, what's disconnected, and
 * what has suspicious types (wildcards on non-wildcard connections, etc.).
 *
 * Output format is designed to be pasted alongside a kismet-analyzer dump
 * for easy side-by-side comparison.
 *
 * Usage:
 *   FGraphValidator Validator;
 *   Validator.DumpBlueprint(NewBP);
 *   // Output goes to LogGraphValidator
 *   // Full dump also written to: {ProjectDir}/Saved/BlueprintUncooker/{BPName}_dump.txt
 */
class FGraphValidator
{
public:
	/**
	 * Dump the complete state of an uncooked Blueprint's graphs.
	 * Logs everything to LogGraphValidator and writes a text file.
	 */
	void DumpBlueprint(UBlueprint* BP);

	/**
	 * Dump a single graph (event graph or function graph).
	 */
	void DumpGraph(UEdGraph* Graph, FString& Out);

	/**
	 * Dump a single node and all its pins.
	 */
	void DumpNode(UEdGraphNode* Node, int32 NodeIndex, FString& Out);

	/**
	 * Dump a single pin with connection info.
	 */
	void DumpPin(UEdGraphPin* Pin, FString& Out);

	/** Get summary statistics after a dump */
	int32 GetTotalNodes() const { return TotalNodes; }
	int32 GetDisconnectedInputs() const { return DisconnectedInputs; }
	int32 GetWildcardConnections() const { return WildcardConnections; }

private:
	/** Get a human-readable description of a pin type */
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	/** Get a short identifier for a node (type + function name) */
	static FString NodeToShortString(UEdGraphNode* Node);

	/** Get the function name from a call function node, if applicable */
	static FString GetNodeFunctionName(UEdGraphNode* Node);

	// Counters
	int32 TotalNodes = 0;
	int32 DisconnectedInputs = 0;
	int32 WildcardConnections = 0;
};
