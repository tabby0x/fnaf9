#pragma once

#include "CoreMinimal.h"
#include "BytecodeReader.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node;
class UBlueprintGeneratedClass;
class UPackage;

// Graph Builder

/**
 * Converts decompiled bytecode IR into UEdGraph nodes within a new UBlueprint.
 *
 * Phase 1 creates nodes for: function calls, variable get/set, branch,
 * sequence, events, return, casts, and basic data wiring.
 *
 * The auto-layout is simple left-to-right flow — functional but not pretty.
 */
class FGraphBuilder
{
public:
	FGraphBuilder();

	/**
	 * Build a complete UBlueprint from decompiled functions.
	 * Creates a new Blueprint in TargetPackage with the given name.
	 * The original class is used for parent class, properties, and component template info.
	 */
	UBlueprint* BuildBlueprint(
		UClass* OriginalClass,
		const TArray<FDecompiledFunction>& Functions,
		UPackage* TargetPackage,
		const FString& BlueprintName);

	/**
	 * Live uncook: build editor graphs onto an existing cooked BlueprintGeneratedClass.
	 * Creates a transient UBlueprint that targets ExistingBPGC as its GeneratedClass.
	 * No compilation needed — the cooked BPGC retains its original bytecode/CDO/properties.
	 * The same UClass* pointer is preserved, so all casts and references stay valid.
	 */
	UBlueprint* LiveBuildBlueprint(
		UClass* OriginalClass,
		const TArray<FDecompiledFunction>& Functions,
		UBlueprintGeneratedClass* ExistingBPGC);

	/**
	 * Child class uncook: build a NEW Blueprint that inherits from the cooked class.
	 * ParentClass = OriginalClass (the cooked BPGC), so the child IS-A original class.
	 * All decompiled logic goes into the child as overrides.
	 * Casts to the original class work because the child inherits from it.
	 * Variables, components, and CDO defaults are inherited — not duplicated.
	 * Caller must compile the returned Blueprint.
	 */
	UBlueprint* BuildChildBlueprint(
		UClass* OriginalClass,
		const TArray<FDecompiledFunction>& Functions,
		UPackage* TargetPackage,
		const FString& BlueprintName);

	const TArray<FString>& GetWarnings() const { return Warnings; }

	// --- Two-pass compile support ---

	/** Snapshot of all pin connections and defaults across all graphs in a Blueprint.
	 *  Used by the two-pass compile: snapshot before pass 1, restore after RefreshAllNodes. */
	struct FPinSnapshot
	{
		FGuid OwningNodeGuid;
		FName PinName;
		EEdGraphPinDirection Direction;
		TArray<TPair<FGuid, FName>> LinkedPinIds; // NodeGuid + PinName of each linked pin
		FString DefaultValue;
		TWeakObjectPtr<UObject> DefaultObject;
		FText DefaultTextValue;
	};

	/** Capture all pin connections and defaults from every graph in the Blueprint. */
	static TArray<FPinSnapshot> SnapshotAllConnections(UBlueprint* BP);

	/** Restore pin connections from a previous snapshot. Logs restored/failed counts. */
	static void RestoreConnectionsFromSnapshot(UBlueprint* BP, const TArray<FPinSnapshot>& Snapshot);

	/** Run post-refresh fixups (wildcard propagation, Select node pins, connection sanitization).
	 *  Normally runs inside BuildBlueprintCore; exposed for the two-pass compile path. */
	static void RunPostRefreshFixups(UBlueprint* BP);

	/** Snapshot of UK2Node_Event EventReference data.
	 *  RefreshAllNodes calls FixupEventReference(true) which corrupts EventReference
	 *  to point to a BPGC skeleton instead of the native C++ declaring class. */
	struct FEventRefSnapshot
	{
		FGuid NodeGuid;
		FName EventName;
		TWeakObjectPtr<UClass> DeclaringClass;
	};

	/** Save EventReference for all UK2Node_Event nodes before RefreshAllNodes. */
	static TArray<FEventRefSnapshot> SnapshotEventReferences(UBlueprint* BP);

	/** Restore EventReference on UK2Node_Event nodes after RefreshAllNodes. */
	static void RestoreEventReferences(UBlueprint* BP, const TArray<FEventRefSnapshot>& Snapshot);

private:
	/** Shared core: build graphs into an already-created UBlueprint.
	 *  Called by BuildBlueprint, LiveBuildBlueprint, and BuildChildBlueprint.
	 *  bIsLiveUncook: skips SetupCDODefaults and MarkBlueprintAsStructurallyModified
	 *  (the live path reuses the existing cooked BPGC which already has valid CDO/properties).
	 *  bIsChildClass: skips SetupVariables, SetupComponents, SetupCDODefaults
	 *  (all inherited from the cooked parent class). */
	bool BuildBlueprintCore(UBlueprint* BP, UClass* OriginalClass, const TArray<FDecompiledFunction>& Functions, bool bIsLiveUncook = false, bool bIsChildClass = false);

	// --- Blueprint setup ---
	void SetupVariables(UBlueprint* BP, UClass* OriginalClass);
	void SetupInterfaces(UBlueprint* BP, UClass* OriginalClass);
	void SetupComponents(UBlueprint* BP, UClass* OriginalClass);
	void SetupCDODefaults(UBlueprint* BP, UClass* OriginalClass);

	// --- Graph building ---

	/** Build the event graph from the ubergraph function */
	void BuildEventGraph(UBlueprint* BP, UEdGraph* EventGraph,
		const FDecompiledFunction& UbergraphFunc,
		const TArray<FDecompiledFunction>& EventStubs,
		UClass* OriginalClass);

	/** Build a standalone function graph */
	void BuildFunctionGraph(UBlueprint* BP, const FDecompiledFunction& Func, UClass* OriginalClass);

	// --- Node creation ---

	/** Create all nodes for a sequence of statements within a graph.
	 *  If pReachableStmts is non-null, only statements whose index is in the set will be processed.
	 *  EventEntryStmtIdx: when >= 0, processing starts from this index (the event's entry point)
	 *  then wraps around to [StartIndex, EventEntryStmtIdx) for backward-jump targets.
	 *  This ensures IncomingExecPin connects to the event entry, not backward-reachable loop bodies. */
	void EmitStatements(
		UEdGraph* Graph,
		const TArray<TSharedPtr<FDecompiledExpr>>& Statements,
		int32 StartIndex, int32 EndIndex,
		int32 BaseX, int32 BaseY,
		UEdGraphPin* IncomingExecPin,
		const TSet<int32>* pReachableStmts = nullptr,
		int32 EventEntryStmtIdx = -1);

	/**
	 * Create a UK2Node for a single expression.
	 * Returns the created node (or nullptr for expressions that don't need nodes).
	 * May recursively create child nodes for sub-expressions.
	 */
	UEdGraphNode* CreateNodeForExpr(
		UEdGraph* Graph,
		TSharedPtr<FDecompiledExpr> Expr,
		int32& X, int32& Y);

	/**
	 * Resolve a data expression to an output pin.
	 * For literals, this sets the pin default value.
	 * For variable gets / function results, this creates a node and returns the output pin.
	 */
	UEdGraphPin* ResolveDataExpr(
		UEdGraph* Graph,
		TSharedPtr<FDecompiledExpr> Expr,
		int32& X, int32& Y);

	// --- Wiring helpers ---
	static bool TryConnect(UEdGraphPin* A, UEdGraphPin* B);

	/** Force-connect two pins using MakeLinkTo (bypasses schema validation).
	 *  Used for context object → Target pin wiring where bytecode guarantees correctness. */
	static bool ForceConnect(UEdGraphPin* A, UEdGraphPin* B);

	/** Check if an expression is a local variable referencing a Temp_ with a stored literal,
	 *  and if so, apply that literal to the given pin. Returns true if handled. */
	bool ApplyTempLiteralToPin(TSharedPtr<FDecompiledExpr> Expr, UEdGraphPin* Pin);
	static UEdGraphPin* FindExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, FName PinName = NAME_None);
	static UEdGraphPin* FindDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, FName PinName);
	static UEdGraphPin* FindFirstDataOutputPin(UEdGraphNode* Node);
	static FName GetPinNameForFunctionParam(UFunction* Function, int32 ParamIndex);

	/** Fix struct pin defaults (Rotator, Vector) that use the named format
	 *  from cooked metadata instead of the numeric format expected by pin import. */
	static void FixStructPinDefaults(UEdGraphNode* Node);

	/** Remove invalid pin connections (direction mismatches) that break the
	 *  compiler's CreateExecutionSchedule topological sort. */
	static void SanitizeGraphConnections(UEdGraph* Graph);

	// --- State ---
	TArray<FString> Warnings;
	UClass* CurrentOriginalClass = nullptr; // Set during BuildBlueprint for virtual function resolution
	bool bIsRedirectedVirtualCall = false; // Set when EX_VirtualFunction redirects to EX_FinalFunction handler
	bool bBuildingChildClass = false; // Set when building a child class — events are overrides, not custom events

	/** Parameter names from event stubs — these get stored to the persistent frame
	 *  and are accessed by implementation functions via EX_LocalOutVariable.
	 *  Used to detect function locals that should be parameters. */
	TSet<FName> EventStubParamNames;

	/** Maps local variable names to the output pin that produced their value.
	 *  This eliminates ghost VariableGet nodes by wiring directly to the
	 *  function call output or FunctionEntry parameter pin. */
	TMap<FName, UEdGraphPin*> LocalVarToPinMap;

	/** Persistent frame variable pins that accumulate across event boundaries.
	 *  When event A writes a variable via EX_LetValueOnPersistentFrame and event B
	 *  reads it (e.g., FinishSpawningActor result used by a separate custom event),
	 *  this map allows cross-event data wiring. NOT cleared by ClearLocalVarMap(). */
	TMap<FName, UEdGraphPin*> PersistentFrameVarMap;

	/** Stores literal expressions for compiler-generated Temp_ variables so they
	 *  can be applied as pin defaults rather than creating ghost VariableGet nodes. */
	TMap<FName, TSharedPtr<FDecompiledExpr>> TempLiteralMap;

	/** Stores literal expressions for function out-parameters (EX_LocalOutVariable)
	 *  whose source is a constant (e.g., EX_ByteConst 1). ResolveDataExpr returns
	 *  nullptr for literals (they don't produce nodes/pins), so the literal value
	 *  must be stored here and applied as a pin default on the FunctionResult node. */
	TMap<FName, TSharedPtr<FDecompiledExpr>> OutParamLiteralMap;

	/** Accumulates struct member writes for the MakeStruct inline pattern.
	 *  Key: local struct variable name (e.g., K2Node_MakeStruct_ConduitBarCustomParams_Struct_4)
	 *  Value: map of member name → source expression
	 *  When the struct variable is consumed (e.g., passed to Array_Set),
	 *  a UK2Node_MakeStruct is created with these member values applied as pin defaults. */
	struct FStructMemberWrite
	{
		UScriptStruct* StructType = nullptr;
		TMap<FName, TSharedPtr<FDecompiledExpr>> MemberValues;
	};
	TMap<FName, FStructMemberWrite> StructMemberAssignMap;

	/** Temp_ variables assigned more than once in the current scope (e.g. loop counters).
	 *  These are excluded from TempLiteralMap because their value changes at runtime. */
	TSet<FName> MultiAssignedTempVars;

	/** Cached TemporaryVariable output pins for multi-assigned Temp_ variables.
	 *  Each multi-assigned Temp_ var gets ONE TemporaryVariable node; all reads share it. */
	TMap<FName, UEdGraphPin*> MultiAssignedTempPins;

	/** Last DynamicCast node created, for auto-mapping bSuccess pins */
	UEdGraphNode* LastCastNode = nullptr;

	/** Last exec output pin from EmitStatements - used to wire FunctionResult exec input */
	UEdGraphPin* LastFunctionExecOut = nullptr;

	/** Exec output pins that lead to EX_Return statements.
	 *  Collected during EmitStatements so BuildFunctionGraph can wire ALL return
	 *  paths to the FunctionResult node's exec input (not just the last one).
	 *  Handles functions with multiple return paths (branches ending in return). */
	TArray<UEdGraphPin*> ReturnExecSources;

	/** Deferred data wire connections for backward-jump bytecode patterns.
	 *  When a function argument references a local variable that hasn't been
	 *  populated in LocalVarToPinMap yet (because the producer statement has a
	 *  higher bytecode offset than the consumer due to backward jumps), we defer
	 *  the connection and resolve it after all statements are processed. */
	TArray<TPair<UEdGraphPin*, FName>> DeferredDataWires;

	/** Deferred pin defaults for multi-assigned AssignmentStatement Value pins.
	 *  RefreshAllNodes() reconstructs all nodes, which can destroy pin DefaultValues
	 *  (especially byte/enum values where "None" maps to NAME_None in FName system).
	 *  We collect these during graph building and re-apply after RefreshAllNodes. */
	struct FDeferredPinDefault
	{
		TWeakObjectPtr<UEdGraphNode> Node;
		FName PinName;
		FString DefaultValue;
		TWeakObjectPtr<UObject> DefaultObject;
	};
	TArray<FDeferredPinDefault> DeferredPinDefaults;

	/** Clear local variable tracking (call between function/event boundaries) */
	void ClearLocalVarMap()
	{
		LocalVarToPinMap.Reset();
		TempLiteralMap.Reset();
		OutParamLiteralMap.Reset();
		StructMemberAssignMap.Reset();
		MultiAssignedTempVars.Reset();
		MultiAssignedTempPins.Reset();
		DeferredDataWires.Reset();
		LastCastNode = nullptr;
		LastFunctionExecOut = nullptr;
		ReturnExecSources.Reset();
	}

	// Layout constants
	static constexpr int32 NodeSpacingX = 350;
	static constexpr int32 NodeSpacingY = 200;
	static constexpr int32 DataNodeOffsetY = -150;

	void Warn(const FString& Message);
};
