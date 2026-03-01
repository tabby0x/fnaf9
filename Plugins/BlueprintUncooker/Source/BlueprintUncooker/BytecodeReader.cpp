#include "BytecodeReader.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/NameTypes.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <memoryapi.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogBytecodeReader, Log, All);

// FDecompiledExpr Implementation

bool FDecompiledExpr::IsLiteral() const
{
	switch (Token)
	{
	case EX_IntConst:
	case EX_Int64Const:
	case EX_UInt64Const:
	case EX_FloatConst:
	case EX_StringConst:
	case EX_UnicodeStringConst:
	case EX_NameConst:
	case EX_ObjectConst:
	case EX_RotationConst:
	case EX_VectorConst:
	case EX_ByteConst:
	case EX_IntConstByte:
	case EX_IntZero:
	case EX_IntOne:
	case EX_True:
	case EX_False:
	case EX_NoObject:
	case EX_Self:
	case EX_TextConst:
		return true;
	default:
		return false;
	}
}

FString FDecompiledExpr::GetLiteralAsString() const
{
	switch (Token)
	{
	case EX_IntConst:
	case EX_IntConstByte:
	case EX_IntZero:
	case EX_IntOne:
		return FString::FromInt(IntValue);
	case EX_Int64Const:
	case EX_UInt64Const:
		return FString::Printf(TEXT("%lld"), Int64Value);
	case EX_FloatConst:
		return FString::SanitizeFloat(FloatValue);
	case EX_StringConst:
	case EX_UnicodeStringConst:
		return StringValue;
	case EX_NameConst:
		return NameValue.ToString();
	case EX_ObjectConst:
		return ObjectRef ? ObjectRef->GetPathName() : TEXT("None");
	case EX_RotationConst:
		return FString::Printf(TEXT("%f,%f,%f"), RotatorValue.Pitch, RotatorValue.Yaw, RotatorValue.Roll);
	case EX_VectorConst:
		return FString::Printf(TEXT("%f,%f,%f"), VectorValue.X, VectorValue.Y, VectorValue.Z);
	case EX_ByteConst:
		return FString::FromInt(ByteValue);
	case EX_True:
		return TEXT("true");
	case EX_False:
		return TEXT("false");
	case EX_NoObject:
		return TEXT("None");
	case EX_Self:
		return TEXT("self");
	default:
		return TEXT("");
	}
}

bool FDecompiledExpr::IsStatement() const
{
	switch (Token)
	{
	case EX_Let:
	case EX_LetObj:
	case EX_LetBool:
	case EX_LetDelegate:
	case EX_LetMulticastDelegate:
	case EX_LetWeakObjPtr:
	case EX_LetValueOnPersistentFrame:
	case EX_Jump:
	case EX_JumpIfNot:
	case EX_Return:
	case EX_PushExecutionFlow:
	case EX_PopExecutionFlow:
	case EX_PopExecutionFlowIfNot:
	case EX_EndOfScript:
	case EX_SwitchValue:
	case EX_Assert:
		return true;
		// Function calls are statements if they're impure (have exec pins)
	case EX_FinalFunction:
	case EX_LocalFinalFunction:
	case EX_VirtualFunction:
	case EX_LocalVirtualFunction:
	case EX_CallMath:
	case EX_CallMulticastDelegate:
		return true; // We'll refine this in the graph builder
		// Delegate operations
	case EX_BindDelegate:
	case EX_AddMulticastDelegate:
	case EX_RemoveMulticastDelegate:
	case EX_ClearMulticastDelegate:
		// Array/set/map operations
	case EX_SetArray:
	case EX_SetSet:
	case EX_SetMap:
		return true;
	default:
		return false;
	}
}

bool FDecompiledExpr::IsFunctionCall() const
{
	switch (Token)
	{
	case EX_FinalFunction:
	case EX_LocalFinalFunction:
	case EX_VirtualFunction:
	case EX_LocalVirtualFunction:
	case EX_CallMath:
	case EX_CallMulticastDelegate:
		return true;
	default:
		return false;
	}
}

FString FDecompiledExpr::ToDebugString() const
{
	FString Result = FString::Printf(TEXT("[%04X] "), StartOffset);

	switch (Token)
	{
	case EX_FinalFunction:
	case EX_LocalFinalFunction:
	case EX_CallMath:
		Result += FString::Printf(TEXT("Call %s (%d args)"),
			FunctionRef ? *FunctionRef->GetName() : TEXT("NULL"), Children.Num());
		break;
	case EX_VirtualFunction:
	case EX_LocalVirtualFunction:
		Result += FString::Printf(TEXT("VirtualCall %s (%d args)"),
			*NameValue.ToString(), Children.Num());
		break;
	case EX_LocalVariable:
		Result += FString::Printf(TEXT("LocalVar %s"), PropertyRef ? *PropertyRef->GetName() : TEXT("NULL"));
		break;
	case EX_InstanceVariable:
		Result += FString::Printf(TEXT("InstanceVar %s"), PropertyRef ? *PropertyRef->GetName() : TEXT("NULL"));
		break;
	case EX_Let:
		Result += TEXT("Let (assign)");
		break;
	case EX_JumpIfNot:
		Result += FString::Printf(TEXT("JumpIfNot -> 0x%04X"), JumpTarget);
		break;
	case EX_Jump:
		Result += FString::Printf(TEXT("Jump -> 0x%04X"), JumpTarget);
		break;
	case EX_Return:
		Result += TEXT("Return");
		break;
	case EX_Context:
		Result += TEXT("Context");
		break;
	case EX_IntConst:
		Result += FString::Printf(TEXT("Int %d"), IntValue);
		break;
	case EX_FloatConst:
		Result += FString::Printf(TEXT("Float %f"), FloatValue);
		break;
	case EX_Self:
		Result += TEXT("Self");
		break;
	case EX_PushExecutionFlow:
		Result += FString::Printf(TEXT("PushExecFlow -> 0x%04X"), JumpTarget);
		break;
	case EX_PopExecutionFlow:
		Result += TEXT("PopExecFlow");
		break;
	case EX_PopExecutionFlowIfNot:
		Result += TEXT("PopExecFlowIfNot");
		break;
	case EX_SwitchValue:
		Result += FString::Printf(TEXT("Switch (%d cases)"), IntValue);
		break;
	case EX_InstanceDelegate:
		Result += FString::Printf(TEXT("InstanceDelegate %s"), *NameValue.ToString());
		break;
	case EX_BindDelegate:
		Result += FString::Printf(TEXT("BindDelegate %s"), *NameValue.ToString());
		break;
	case EX_IntConstByte:
		Result += FString::Printf(TEXT("IntByte %d"), IntValue);
		break;
	case EX_LetObj:
		Result += TEXT("LetObj");
		break;
	case EX_LetBool:
		Result += TEXT("LetBool");
		break;
	case EX_LetWeakObjPtr:
		Result += TEXT("LetWeakObj");
		break;
	case EX_LetDelegate:
		Result += TEXT("LetDelegate");
		break;
	case EX_LetMulticastDelegate:
		Result += TEXT("LetMulticastDelegate");
		break;
	case EX_CallMulticastDelegate:
		Result += FString::Printf(TEXT("CallMulticastDelegate (%d args)"), Children.Num());
		break;
	default:
		Result += FString::Printf(TEXT("Token 0x%02X"), (uint8)Token);
		break;
	}

	return Result;
}

// FBytecodeReader Implementation

FBytecodeReader::FBytecodeReader()
	: Script(nullptr)
	, Offset(0)
	, bReadError(false)
{
}

bool FBytecodeReader::HasMore() const
{
	return Script && Offset < Script->Num();
}

EExprToken FBytecodeReader::PeekToken() const
{
	if (HasMore())
	{
		return (EExprToken)(*Script)[Offset];
	}
	return EX_EndOfScript;
}

void FBytecodeReader::LogError(const FString& Message)
{
	Errors.Add(Message);
	UE_LOG(LogBytecodeReader, Warning, TEXT("%s"), *Message);
}

FName FBytecodeReader::ReadScriptName()
{
	/* Script names are serialized as FScriptName in bytecode:
	   { ComparisonIndex(4), DisplayIndex(4), Number(4) } = 12 bytes. */
	FScriptName ScriptName;
	FMemory::Memcpy(&ScriptName, &(*Script)[Offset], sizeof(FScriptName));
	Offset += sizeof(FScriptName);

	return ScriptNameToName(ScriptName);
}

FString FBytecodeReader::ReadAnsiString()
{
	FString Result;
	while (HasMore() && (*Script)[Offset] != 0)
	{
		Result += (ANSICHAR)(*Script)[Offset];
		Offset++;
	}
	if (HasMore()) Offset++; // skip null terminator
	return Result;
}

FString FBytecodeReader::ReadUnicodeString()
{
	FString Result;
	while (HasMore() && !bReadError && (Offset + (int32)sizeof(uint16)) <= Script->Num())
	{
		uint16 Char;
		FMemory::Memcpy(&Char, &(*Script)[Offset], sizeof(uint16));
		Offset += sizeof(uint16);
		if (Char == 0) break;
		Result += (TCHAR)Char;
	}
	return Result;
}

// Static helpers for identifying ubergraph/event stubs

UFunction* FBytecodeReader::FindUbergraphFunction(UClass* Class)
{
	FString UbergraphName = FString::Printf(TEXT("ExecuteUbergraph_%s"), *Class->GetName());
	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->GetName() == UbergraphName || It->GetName().StartsWith(TEXT("ExecuteUbergraph_")))
		{
			return *It;
		}
	}
	return nullptr;
}

bool FBytecodeReader::IsEventStubFunction(UFunction* Function, UClass* OwnerClass, int32& OutUbergraphOffset)
{
	OutUbergraphOffset = -1;

	const TArray<uint8>& FuncScript = Function->Script;
	if (FuncScript.Num() < 2)
	{
		return false;
	}

	/* Validate a pointer value BEFORE dereferencing it. When scanning bytecode we may
	   read random data bytes as pointers. Uses VirtualQuery to verify the memory page
	   is readable, then checks UObject validity through the GC array. */
	auto IsLikelyValidUObject = [](const void* Ptr) -> bool
		{
			if (!Ptr) return false;
			UPTRINT Val = (UPTRINT)Ptr;
			if ((Val & 7) != 0) return false;           // Must be 8-byte aligned
			if (Val < 0x10000) return false;             // Not in low memory
#if PLATFORM_64BITS
			if (Val > 0x00007FFFFFFFFFFF) return false;  // Not in kernel space (Windows x64)
#endif
			// Check if memory is actually readable using OS page query
			MEMORY_BASIC_INFORMATION MemInfo;
			if (VirtualQuery(Ptr, &MemInfo, sizeof(MemInfo)) == 0)
				return false;
			// Page must be committed (not reserved/free) and readable
			if (MemInfo.State != MEM_COMMIT)
				return false;
			const DWORD ReadableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
			if ((MemInfo.Protect & ReadableFlags) == 0)
				return false;
			// Memory is readable — now verify it's a real GC-tracked UObject
			const UObject* Obj = static_cast<const UObject*>(Ptr);
			return Obj->IsValidLowLevel();
		};

	// Helper: try to read an ExecuteUbergraph call at a specific bytecode offset.
	// Returns true if a valid call was found and OutUbergraphOffset was set.
	auto TryReadUbergraphCall = [&](int32 ReadPos) -> bool
		{
			EExprToken Token = (EExprToken)FuncScript[ReadPos];
			if (Token != EX_LocalFinalFunction && Token != EX_FinalFunction && Token != EX_CallMath)
				return false;

			if (ReadPos + 1 + (int32)sizeof(UFunction*) > FuncScript.Num())
				return false;

			UFunction* CalledFunc = nullptr;
			FMemory::Memcpy(&CalledFunc, &FuncScript[ReadPos + 1], sizeof(UFunction*));

			// Validate pointer: check memory is readable, then verify GC-tracked UObject
			if (!IsLikelyValidUObject(CalledFunc))
				return false;

			if (!CalledFunc->GetName().StartsWith(TEXT("ExecuteUbergraph_")))
				return false;

			// Found the call � now read the integer offset argument
			int32 ArgOffset = ReadPos + 1 + sizeof(UFunction*);
			while (ArgOffset < FuncScript.Num())
			{
				EExprToken ArgToken = (EExprToken)FuncScript[ArgOffset];
				if (ArgToken == EX_IntConst && ArgOffset + 5 <= FuncScript.Num())
				{
					FMemory::Memcpy(&OutUbergraphOffset, &FuncScript[ArgOffset + 1], sizeof(int32));
					return true;
				}
				else if (ArgToken == EX_IntConstByte && ArgOffset + 2 <= FuncScript.Num())
				{
					OutUbergraphOffset = FuncScript[ArgOffset + 1];
					return true;
				}
				else if (ArgToken == EX_EndFunctionParms)
				{
					break;
				}
				else
				{
					ArgOffset++;
				}
			}
			// Found ExecuteUbergraph call but couldn't parse offset
			if (OutUbergraphOffset < 0) OutUbergraphOffset = 0;
			return true;
		};

	// ---- Pattern 1: Direct function call at offset 0 ----
	// EX_LocalFinalFunction [ptr] EX_IntConst [offset] EX_EndFunctionParms EX_Return ...
	EExprToken FirstToken = (EExprToken)FuncScript[0];
	if (FirstToken == EX_LocalFinalFunction || FirstToken == EX_FinalFunction || FirstToken == EX_CallMath)
	{
		return TryReadUbergraphCall(0);
	}

	/* Pattern 2: Parameter copies before function call.
	   Stubs with params start with EX_LetValueOnPersistentFrame to copy arguments.
	   We scan forward looking for a function call token. */
	if (FirstToken == EX_LetValueOnPersistentFrame)
	{
		// Event stubs are short � skip scanning huge functions
		if (FuncScript.Num() > 512) return false;

		for (int32 SearchPos = 1; SearchPos < FuncScript.Num(); SearchPos++)
		{
			EExprToken Token = (EExprToken)FuncScript[SearchPos];
			if (Token == EX_LocalFinalFunction || Token == EX_FinalFunction || Token == EX_CallMath)
			{
				if (TryReadUbergraphCall(SearchPos))
					return true;
				// If TryReadUbergraphCall failed (pointer validation failed or wrong function),
				// continue scanning � the byte was just data that matched the token value
			}
		}
	}

	return false;
}

// Class-level decompilation

TArray<FDecompiledFunction> FBytecodeReader::DecompileClass(UClass* Class)
{
	TArray<FDecompiledFunction> Results;
	if (!Class) return Results;

	// Find the ubergraph function
	UFunction* UbergraphFunc = FindUbergraphFunction(Class);

	// First pass: identify event stubs and collect their entry offsets
	TMap<FName, int32> EventEntryOffsets; // Event name -> ubergraph offset
	TArray<UFunction*> EventStubs;

	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		UFunction* Func = *It;
		int32 UbergraphOffset = -1;

		if (Func == UbergraphFunc) continue; // Don't process ubergraph as a normal function

		if (IsEventStubFunction(Func, Class, UbergraphOffset))
		{
			EventEntryOffsets.Add(Func->GetFName(), UbergraphOffset);
			EventStubs.Add(Func);

			// Still add a stub entry to results for reference
			FDecompiledFunction StubResult;
			StubResult.OriginalFunction = Func;
			StubResult.FunctionName = Func->GetName();
			StubResult.bIsEventStub = true;
			StubResult.UbergraphEntryOffset = UbergraphOffset;
			Results.Add(MoveTemp(StubResult));

			UE_LOG(LogBytecodeReader, Verbose, TEXT("Event stub: %s -> ubergraph offset 0x%04X"),
				*Func->GetName(), UbergraphOffset);
		}
		else
		{
			// Regular function - decompile it
			if (Func->Script.Num() > 0)
			{
				FDecompiledFunction Decompiled = DecompileFunction(Func);
				Results.Add(MoveTemp(Decompiled));
			}
		}
	}

	// Decompile the ubergraph with event entry point information
	if (UbergraphFunc && UbergraphFunc->Script.Num() > 0)
	{
		FDecompiledFunction UbergraphResult = DecompileFunction(UbergraphFunc);
		UbergraphResult.bIsUbergraph = true;
		UbergraphResult.EventEntryPoints = EventEntryOffsets;

		UE_LOG(LogBytecodeReader, Log, TEXT("Ubergraph: %s (%d bytes, %d statements, %d entry points)"),
			*UbergraphFunc->GetName(), UbergraphFunc->Script.Num(),
			UbergraphResult.Statements.Num(), EventEntryOffsets.Num());

		Results.Add(MoveTemp(UbergraphResult));
	}

	return Results;
}

// Function-level decompilation

FDecompiledFunction FBytecodeReader::DecompileFunction(UFunction* Function)
{
	FDecompiledFunction Result;
	Result.OriginalFunction = Function;
	Result.FunctionName = Function->GetName();

	Script = &Function->Script;
	Offset = 0;
	bReadError = false;

	UE_LOG(LogBytecodeReader, Log, TEXT("Decompiling %s (%d bytes)"),
		*Function->GetName(), Script->Num());

	int32 StatementIndex = 0;
	while (HasMore() && !bReadError)
	{
		int32 StatementOffset = Offset;

		TSharedPtr<FDecompiledExpr> Expr = ReadExpression();
		if (!Expr || bReadError) break;

		// Track offset -> statement mapping for jump resolution
		Result.OffsetToStatementIndex.Add(StatementOffset, StatementIndex);

		Result.Statements.Add(Expr);
		StatementIndex++;

		// Debug log
		UE_LOG(LogBytecodeReader, Verbose, TEXT("  %s"), *Expr->ToDebugString());

		// Stop at end of script
		if (Expr->Token == EX_EndOfScript) break;
	}

	if (bReadError)
	{
		LogError(FString::Printf(TEXT("Read error in function %s at offset %d/%d. %d statements recovered before error."),
			*Function->GetName(), Offset, Script->Num(), Result.Statements.Num()));
	}

	Script = nullptr;
	return Result;
}

// Recursive expression reader

TSharedPtr<FDecompiledExpr> FBytecodeReader::ReadExpression()
{
	if (!HasMore() || bReadError)
	{
		return nullptr;
	}

	auto Expr = MakeShared<FDecompiledExpr>();
	Expr->StartOffset = Offset;
	Expr->Token = (EExprToken)Read<uint8>();

	switch (Expr->Token)
	{

		// TERMINATORS / NO-OPS
	case EX_Nothing:
	case EX_EndOfScript:
	case EX_EndFunctionParms:
	case EX_EndStructConst:
	case EX_EndArray:
	case EX_EndArrayConst:
	case EX_EndSet:
	case EX_EndMap:
	case EX_EndSetConst:
	case EX_EndMapConst:
		break;

		// INTEGER LITERALS
	case EX_IntConst:
		Expr->IntValue = Read<int32>();
		break;

	case EX_Int64Const:
		Expr->Int64Value = Read<int64>();
		break;

	case EX_UInt64Const:
		Expr->Int64Value = (int64)Read<uint64>();
		break;

	case EX_IntZero:
		Expr->IntValue = 0;
		break;

	case EX_IntOne:
		Expr->IntValue = 1;
		break;

	case EX_ByteConst:
		Expr->ByteValue = Read<uint8>();
		break;

	case EX_IntConstByte:
		Expr->IntValue = Read<uint8>(); // byte value stored as int
		break;

		// FLOAT LITERALS
	case EX_FloatConst:
		Expr->FloatValue = Read<float>();
		break;

		// STRING / NAME / TEXT LITERALS
	case EX_StringConst:
		Expr->StringValue = ReadAnsiString();
		break;

	case EX_UnicodeStringConst:
		Expr->StringValue = ReadUnicodeString();
		break;

	case EX_NameConst:
		Expr->NameValue = ReadScriptName();
		break;

	case EX_TextConst:
	{
		uint8 TextLitType = Read<uint8>();
		Expr->ByteValue = TextLitType;
		switch (TextLitType)
		{
		case 0: // Empty
			break;
		case 1: // LocalizedText
			Expr->Children.Add(ReadExpression()); // source string
			Expr->Children.Add(ReadExpression()); // key
			Expr->Children.Add(ReadExpression()); // namespace
			break;
		case 2: // InvariantText
			Expr->Children.Add(ReadExpression()); // source string
			break;
		case 3: // LiteralString
			Expr->Children.Add(ReadExpression()); // source string
			break;
		case 4: // StringTableEntry
			Expr->ObjectRef = ReadObject(); // string table asset
			Expr->Children.Add(ReadExpression()); // table id
			Expr->Children.Add(ReadExpression()); // key
			break;
		default:
			LogError(FString::Printf(TEXT("Unknown text literal type: %d at offset %d"), TextLitType, Expr->StartOffset));
			break;
		}
		break;
	}

		// BOOLEAN LITERALS
	case EX_True:
		Expr->BoolValue = true;
		break;

	case EX_False:
		Expr->BoolValue = false;
		break;

		// OBJECT LITERALS
	case EX_ObjectConst:
		Expr->ObjectRef = ReadObject();
		break;

	case EX_SoftObjectConst:
		Expr->Children.Add(ReadExpression()); // path string expression
		break;

	case EX_NoObject:
		break;

	case EX_NoInterface:
		break;

		// STRUCT / VECTOR / ROTATOR LITERALS
	case EX_VectorConst:
		Expr->VectorValue.X = Read<float>();
		Expr->VectorValue.Y = Read<float>();
		Expr->VectorValue.Z = Read<float>();
		break;

	case EX_RotationConst:
		Expr->RotatorValue.Pitch = Read<float>();
		Expr->RotatorValue.Yaw = Read<float>();
		Expr->RotatorValue.Roll = Read<float>();
		break;

	case EX_TransformConst:
	{
		/* In bytecode, FTransform is serialized as Rotation(Quat4f=16) + Translation(Vec3f=12)
		   + Scale(Vec3f=12) = 40 bytes. We read component by component. */
		Expr->RotatorValue.Pitch = Read<float>(); // Quat X (we're abusing RotatorValue for storage)
		Expr->RotatorValue.Yaw = Read<float>();   // Quat Y
		Expr->RotatorValue.Roll = Read<float>();   // Quat Z
		Read<float>(); // Quat W
		Expr->VectorValue.X = Read<float>(); // Translation X
		Expr->VectorValue.Y = Read<float>(); // Translation Y
		Expr->VectorValue.Z = Read<float>(); // Translation Z
		Read<float>(); Read<float>(); Read<float>(); // Scale3D
		break;
	}

	case EX_StructConst:
	{
		Expr->StructRef = (UScriptStruct*)ReadObject();
		int32 SerializedSize = Read<int32>();
		// Read struct member expressions until EX_EndStructConst
		while (HasMore() && !bReadError && PeekToken() != EX_EndStructConst)
		{
			auto Child = ReadExpression();
			if (Child)
				Expr->Children.Add(Child);
			else
				break;
		}
		if (HasMore()) ReadExpression(); // consume EX_EndStructConst
		break;
	}

		// SELF REFERENCE
	case EX_Self:
		break;

		// VARIABLE ACCESS
	case EX_LocalVariable:
	case EX_InstanceVariable:
	case EX_DefaultVariable:
	case EX_LocalOutVariable:
		Expr->PropertyRef = ReadProperty();
		break;

	case EX_StructMemberContext:
		Expr->PropertyRef = ReadProperty();
		Expr->Children.Add(ReadExpression()); // struct owner expression
		break;

		// FUNCTION CALLS (with resolved function pointer)
	case EX_FinalFunction:
	case EX_LocalFinalFunction:
	case EX_CallMath:
	{
		Expr->FunctionRef = (UFunction*)ReadObject();
		// Read arguments until EX_EndFunctionParms
		while (HasMore() && !bReadError && PeekToken() != EX_EndFunctionParms)
		{
			auto Arg = ReadExpression();
			if (Arg)
				Expr->Children.Add(Arg);
			else
				break;
		}
		if (HasMore()) ReadExpression(); // consume EX_EndFunctionParms
		break;
	}

	// FUNCTION CALLS (with name-based resolution)
	case EX_VirtualFunction:
	case EX_LocalVirtualFunction:
	{
		Expr->NameValue = ReadScriptName();
		while (HasMore() && !bReadError && PeekToken() != EX_EndFunctionParms)
		{
			auto Arg = ReadExpression();
			if (Arg)
				Expr->Children.Add(Arg);
			else
				break;
		}
		if (HasMore()) ReadExpression(); // consume EX_EndFunctionParms
		break;
	}

	// MULTICAST DELEGATE CALL
	case EX_CallMulticastDelegate:
	{
		Expr->FunctionRef = (UFunction*)ReadObject(); // signature function
		while (HasMore() && !bReadError && PeekToken() != EX_EndFunctionParms)
		{
			auto Arg = ReadExpression();
			if (Arg)
				Expr->Children.Add(Arg);
			else
				break;
		}
		if (HasMore()) ReadExpression();
		break;
	}

		// ASSIGNMENTS (Let variants)
	case EX_Let:
		Expr->PropertyRef = ReadProperty(); // type hint
		Expr->Children.Add(ReadExpression()); // destination variable
		Expr->Children.Add(ReadExpression()); // source value
		break;

	case EX_LetObj:
	case EX_LetBool:
	case EX_LetDelegate:
	case EX_LetMulticastDelegate:
	case EX_LetWeakObjPtr:
		Expr->Children.Add(ReadExpression()); // destination variable
		Expr->Children.Add(ReadExpression()); // source value
		break;

	case EX_LetValueOnPersistentFrame:
		Expr->PropertyRef = ReadProperty(); // destination property
		Expr->Children.Add(ReadExpression()); // source value
		break;

		// CONTROL FLOW
	case EX_Jump:
		Expr->JumpTarget = ReadSkipCount();
		break;

	case EX_JumpIfNot:
		Expr->JumpTarget = ReadSkipCount(); // offset of else/end
		Expr->Children.Add(ReadExpression()); // condition expression
		break;

	case EX_Return:
		Expr->Children.Add(ReadExpression()); // return value (often EX_Nothing)
		break;

	case EX_PushExecutionFlow:
		Expr->JumpTarget = ReadSkipCount(); // offset to resume at
		break;

	case EX_PopExecutionFlow:
		break; // resumes at pushed address

	case EX_PopExecutionFlowIfNot:
		Expr->Children.Add(ReadExpression()); // condition
		break;

	case EX_ComputedJump:
		Expr->Children.Add(ReadExpression()); // index expression
		break;

	case EX_SwitchValue:
	{
		uint16 NumCases = Read<uint16>();         // case count comes FIRST
		uint32 EndGoto = ReadSkipCount();         // end-of-switch jump offset comes SECOND
		Expr->IntValue = NumCases;
		Expr->JumpTarget = EndGoto;
		Expr->Children.Add(ReadExpression()); // index/switch expression

		for (int32 i = 0; i < NumCases; i++)
		{
			Expr->Children.Add(ReadExpression()); // case value
			uint32 CaseSkip = ReadSkipCount();    // code offset for this case
			Expr->Children.Add(ReadExpression()); // case result
		}
		Expr->Children.Add(ReadExpression()); // default value
		break;
	}

	case EX_Assert:
	{
		uint16 LineNumber = Read<uint16>();
		uint8 InDebugMode = Read<uint8>();
		Expr->IntValue = LineNumber;
		Expr->ByteValue = InDebugMode;
		Expr->Children.Add(ReadExpression()); // condition
		break;
	}

	case EX_Skip:
	{
		uint32 SkipCount = ReadSkipCount();
		Expr->JumpTarget = SkipCount;
		Expr->Children.Add(ReadExpression()); // skipped expression
		break;
	}

		// CONTEXT (calling functions on other objects)
	case EX_Context:
	case EX_Context_FailSilent:
	{
		Expr->ContextObject = ReadExpression();    // target object expression
		uint32 SkipBytes = ReadSkipCount();        // bytes to skip if null
		Expr->FieldRef = ReadField();              // R-value property (for property access context)
		Expr->Children.Add(ReadExpression());      // the operation to execute in this context
		Expr->JumpTarget = SkipBytes;              // store skip for reference
		break;
	}

	case EX_InterfaceContext:
		Expr->Children.Add(ReadExpression()); // interface expression
		break;

		// CASTS
	case EX_DynamicCast:
		Expr->ClassRef = (UClass*)ReadObject();
		Expr->Children.Add(ReadExpression()); // object to cast
		break;

	case EX_MetaCast:
		Expr->ClassRef = (UClass*)ReadObject();
		Expr->Children.Add(ReadExpression()); // class object to cast
		break;

	case EX_PrimitiveCast:
	{
		uint8 ConversionType = Read<uint8>();
		Expr->ByteValue = ConversionType;
		Expr->Children.Add(ReadExpression()); // value to convert
		break;
	}

	case EX_ObjToInterfaceCast:
	case EX_CrossInterfaceCast:
	case EX_InterfaceToObjCast:
		Expr->ClassRef = (UClass*)ReadObject();
		Expr->Children.Add(ReadExpression());
		break;

		// DELEGATE OPERATIONS
	case EX_BindDelegate:
		Expr->NameValue = ReadScriptName();     // function name to bind
		Expr->Children.Add(ReadExpression());   // delegate expression
		Expr->Children.Add(ReadExpression());   // target object
		break;

	case EX_AddMulticastDelegate:
	case EX_RemoveMulticastDelegate:
		Expr->Children.Add(ReadExpression()); // multicast delegate
		Expr->Children.Add(ReadExpression()); // delegate to add/remove
		break;

	case EX_ClearMulticastDelegate:
		Expr->Children.Add(ReadExpression()); // multicast delegate to clear
		break;

	case EX_InstanceDelegate:
		Expr->NameValue = ReadScriptName(); // delegate function name
		break;

		// ARRAY OPERATIONS
	case EX_SetArray:
	{
		// First child is the assignment target (the array property expression)
		Expr->Children.Add(ReadExpression());
		// Then elements until EX_EndArray
		while (HasMore() && !bReadError && PeekToken() != EX_EndArray)
		{
			auto Elem = ReadExpression();
			if (Elem)
				Expr->Children.Add(Elem);
			else
				break;
		}
		if (HasMore()) ReadExpression(); // consume EX_EndArray
		break;
	}

	case EX_ArrayConst:
	{
		Expr->PropertyRef = ReadProperty(); // inner property type
		int32 NumElements = Read<int32>();
		Expr->IntValue = NumElements;
		for (int32 i = 0; i < NumElements; i++)
		{
			Expr->Children.Add(ReadExpression());
		}
		break;
	}

	case EX_SetSet:
	{
		Expr->Children.Add(ReadExpression()); // set property
		int32 NumElements = Read<int32>();
		for (int32 i = 0; i < NumElements; i++)
		{
			Expr->Children.Add(ReadExpression());
		}
		break;
	}

	case EX_SetMap:
	{
		Expr->Children.Add(ReadExpression()); // map property
		int32 NumElements = Read<int32>();
		for (int32 i = 0; i < NumElements; i++)
		{
			Expr->Children.Add(ReadExpression()); // key
			Expr->Children.Add(ReadExpression()); // value
		}
		break;
	}

	case EX_SetConst:
	{
		Expr->PropertyRef = ReadProperty(); // inner property
		int32 NumElements = Read<int32>();
		Expr->IntValue = NumElements;
		for (int32 i = 0; i < NumElements; i++)
		{
			Expr->Children.Add(ReadExpression());
		}
		break;
	}

	case EX_MapConst:
	{
		Expr->PropertyRef = ReadProperty(); // key property
		Expr->FieldRef = ReadField();       // value property
		int32 NumElements = Read<int32>();
		Expr->IntValue = NumElements;
		for (int32 i = 0; i < NumElements; i++)
		{
			Expr->Children.Add(ReadExpression()); // key
			Expr->Children.Add(ReadExpression()); // value
		}
		break;
	}

		// DEBUGGING / INSTRUMENTATION (skip gracefully)
	case EX_Tracepoint:
	case EX_WireTracepoint:
		break; // no data

	case EX_InstrumentationEvent:
	{
		uint8 EventType = Read<uint8>();
		Expr->ByteValue = EventType;
		// InlineEvent type (2) also has a name
		if (EventType == 2) // EScriptInstrumentationType::InlineEvent
		{
			Expr->NameValue = ReadScriptName();
		}
		break;
	}

	case EX_Breakpoint:
		break; // no data, editor-only (but might appear)		// MISC
	case EX_ClassContext:
	{
		Expr->ContextObject = ReadExpression(); // class expression
		uint32 SkipBytes = ReadSkipCount();
		Expr->FieldRef = ReadField();
		Expr->Children.Add(ReadExpression()); // context expression
		Expr->JumpTarget = SkipBytes;
		break;
	}

	case EX_PropertyConst:
		Expr->PropertyRef = ReadProperty();
		break;

	case EX_SkipOffsetConst:
		Expr->JumpTarget = ReadSkipCount(); // code skip offset constant
		break;

	case EX_ArrayGetByRef:
		Expr->Children.Add(ReadExpression()); // array expression
		Expr->Children.Add(ReadExpression()); // index expression
		break;

	case EX_ClassSparseDataVariable:
		Expr->PropertyRef = ReadProperty(); // sparse data property
		break;

	case EX_FieldPathConst:
		Expr->PropertyRef = ReadProperty(); // field path property
		break;

	default:
		/* Custom CAS Engine tokens (Steel Wool Studios UE4 fork).
		   Standard UE4.27 tokens end at ~0x6D, UE5 extensions go to ~0x74. All confirmed
		   CAS tokens are > 0x74 (0x78, 0x93, 0xA6, 0xB0, 0xB1, 0xF6). Format: int32 +
		   UObject* pointer (12 bytes total). Low-range tokens hitting default indicate
		   upstream parser bugs (misaligned reads) -- treating them as 12-byte CAS tokens
		   would silently consume valid bytecode. We skip the pointer bytes without forming
		   a UObject*. */
		if ((uint8)Expr->Token > 0x74 && Script && (Offset + 12) <= Script->Num())
		{
			Expr->IntValue = Read<int32>();       // 4 bytes: unknown flags/index
			Offset += 8;                          // 8 bytes: skip object pointer (do NOT dereference)
			Expr->FunctionRef = nullptr;
			UE_LOG(LogBytecodeReader, Verbose, TEXT("Custom CAS token 0x%02X at offset %d: int=%d (pointer skipped for safety)"),
				(uint8)Expr->Token, Expr->StartOffset, Expr->IntValue);
			break;
		}

		// Unhandled token � log context and abort this function
		{
			FString ByteDump;
			int32 DumpStart = FMath::Max(0, Expr->StartOffset - 8);
			int32 DumpEnd = FMath::Min(Script->Num(), Expr->StartOffset + 16);
			for (int32 i = DumpStart; i < DumpEnd; i++)
			{
				if (i == Expr->StartOffset) ByteDump += TEXT("[");
				ByteDump += FString::Printf(TEXT("%02X"), (*Script)[i]);
				if (i == Expr->StartOffset) ByteDump += TEXT("]");
				ByteDump += TEXT(" ");
			}
			LogError(FString::Printf(TEXT("UNHANDLED EExprToken: 0x%02X at offset %d (total %d bytes). Bytes: %s"),
				(uint8)Expr->Token, Expr->StartOffset, Script->Num(), *ByteDump));
		}
		bReadError = true;
		Offset = Script ? Script->Num() : 0;
		return nullptr;
	}

	Expr->EndOffset = Offset;
	return Expr;
}