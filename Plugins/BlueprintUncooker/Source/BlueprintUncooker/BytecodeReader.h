#pragma once

#include "CoreMinimal.h"
#include "UObject/Script.h"

class UFunction;
class UClass;
class UScriptStruct;

// Intermediate Representation types

/**
 * Represents a single decoded bytecode expression.
 * Expressions can be nested (function call arguments, assignment operands, etc.)
 */
struct FDecompiledExpr : public TSharedFromThis<FDecompiledExpr>
{
	EExprToken Token;
	int32 StartOffset = 0; // Byte offset where this expression starts in UFunction::Script
	int32 EndOffset = 0;   // Byte offset where this expression ends

	// --- Object references (populated based on Token) ---
	UFunction* FunctionRef = nullptr;
	FProperty* PropertyRef = nullptr;
	FField* FieldRef = nullptr;
	UObject* ObjectRef = nullptr;
	UClass* ClassRef = nullptr;
	UScriptStruct* StructRef = nullptr;

	// --- Literal values ---
	int32 IntValue = 0;
	int64 Int64Value = 0;
	uint8 ByteValue = 0;
	float FloatValue = 0.f;
	FString StringValue;
	FName NameValue;
	bool BoolValue = false;
	FVector VectorValue = FVector::ZeroVector;
	FRotator RotatorValue = FRotator::ZeroRotator;

	// --- Control flow ---
	uint32 JumpTarget = 0; // Target bytecode offset for jumps

	// --- Nested expressions ---
	TArray<TSharedPtr<FDecompiledExpr>> Children; // Function args, operands, etc.
	TSharedPtr<FDecompiledExpr> ContextObject;    // For EX_Context: the target object

	/** Is this a literal/constant that maps to a pin default value? */
	bool IsLiteral() const;

	/** Get the literal value as a string suitable for pin defaults */
	FString GetLiteralAsString() const;

	/** Does this expression represent an execution-flow statement (not just a data expression)? */
	bool IsStatement() const;

	/** Is this any kind of function call? */
	bool IsFunctionCall() const;

	/** Get a human-readable debug description */
	FString ToDebugString() const;
};

/**
 * Represents one decompiled function with statements organized for graph building
 */
struct FDecompiledFunction
{
	UFunction* OriginalFunction = nullptr;
	FString FunctionName;

	// Top-level statements in bytecode order
	TArray<TSharedPtr<FDecompiledExpr>> Statements;

	// Map bytecode offsets to statement indices (for jump resolution)
	TMap<int32, int32> OffsetToStatementIndex;

	// For ubergraph: maps event names to entry offsets
	TMap<FName, int32> EventEntryPoints;

	// Flags
	bool bIsUbergraph = false;
	bool bIsEventStub = false;
	int32 UbergraphEntryOffset = -1; // If this is an event stub, the offset into the ubergraph
};

// Bytecode Reader

/**
 * Reads UE4 kismet bytecode from UFunction::Script and produces a decompiled IR.
 * This is essentially the inverse of FKismetCompilerVMBackend.
 *
 * Phase 1 supports: function calls, variable access, literals, assignments,
 * basic control flow (branch, jump, sequence), casts, and context expressions.
 */
class FBytecodeReader
{
public:
	FBytecodeReader();

	/** Decompile all functions from a UBlueprintGeneratedClass */
	TArray<FDecompiledFunction> DecompileClass(UClass* Class);

	/** Decompile a single UFunction */
	FDecompiledFunction DecompileFunction(UFunction* Function);

	/** Access errors encountered during reading */
	const TArray<FString>& GetErrors() const { return Errors; }

private:
	// --- Reading state ---
	const TArray<uint8>* Script;
	int32 Offset;
	bool bReadError; // Set when we run off the end or hit something unexpected

	// --- Error tracking ---
	TArray<FString> Errors;

	// --- Expression reader (recursive descent) ---
	TSharedPtr<FDecompiledExpr> ReadExpression();

	// --- Typed read helpers ---
	template<typename T>
	T Read()
	{
		T Value{};
		if (!Script || (Offset + (int32)sizeof(T)) > Script->Num())
		{
			LogError(FString::Printf(TEXT("Read<> out of bounds: offset %d + %d > %d"),
				Offset, (int32)sizeof(T), Script ? Script->Num() : 0));
			bReadError = true;
			return Value;
		}
		FMemory::Memcpy(&Value, &(*Script)[Offset], sizeof(T));
		Offset += sizeof(T);
		return Value;
	}

	UObject* ReadObject()
	{
		if (bReadError) return nullptr;
		return Read<UObject*>();
	}
	FField* ReadField()
	{
		if (bReadError) return nullptr;
		return Read<FField*>();
	}
	FProperty* ReadProperty()
	{
		return (FProperty*)ReadField();
	}
	uint32 ReadSkipCount()
	{
		if (bReadError) return 0;
		return Read<uint32>();
	}

	FName ReadScriptName();
	FString ReadAnsiString();
	FString ReadUnicodeString();

	// --- Utilities ---
	bool HasMore() const;
	EExprToken PeekToken() const;
	void LogError(const FString& Message);

	/** Check if a function is an event stub that just calls ExecuteUbergraph */
	static bool IsEventStubFunction(UFunction* Function, UClass* OwnerClass, int32& OutUbergraphOffset);

	/** Find the ExecuteUbergraph function in a class */
	static UFunction* FindUbergraphFunction(UClass* Class);
};