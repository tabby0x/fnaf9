#include "BlueprintUncookerModule.h"
#include "BytecodeReader.h"
#include "GraphBuilder.h"
#include "GraphValidator.h"

#include "ContentBrowserModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "BlueprintUncooker"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintUncooker, Log, All);

IMPLEMENT_MODULE(FBlueprintUncookerModule, BlueprintUncooker)

void FBlueprintUncookerModule::StartupModule()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FBlueprintUncookerModule::ExtendContentBrowserMenu));
	ContentBrowserExtenderHandle = Extenders.Last().GetHandle();

	UE_LOG(LogBlueprintUncooker, Log, TEXT("Module loaded. Right-click a Blueprint -> Uncook Blueprint"));
}

void FBlueprintUncookerModule::ShutdownModule()
{
	if (ContentBrowserExtenderHandle.IsValid())
	{
		FContentBrowserModule* ContentBrowserModule = FModuleManager::GetModulePtr<FContentBrowserModule>("ContentBrowser");
		if (ContentBrowserModule)
		{
			TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule->GetAllAssetViewContextMenuExtenders();
			Extenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
				{
					return Delegate.GetHandle() == ContentBrowserExtenderHandle;
				});
		}
	}
}

TSharedRef<FExtender> FBlueprintUncookerModule::ExtendContentBrowserMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	// Check if any selected asset is a Blueprint or BlueprintGeneratedClass (cooked)
	bool bHasBlueprint = false;
	for (const FAssetData& Asset : SelectedAssets)
	{
		if (Asset.GetClass()->IsChildOf(UBlueprint::StaticClass()))
		{
			bHasBlueprint = true;
			break;
		}

		if (Asset.GetClass()->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
		{
			bHasBlueprint = true;
			break;
		}

		// Also check by asset class name for cooked assets where class might not resolve
		FString ClassName = Asset.AssetClass.ToString();
		if (ClassName.Contains(TEXT("Blueprint")) || ClassName.Contains(TEXT("_C")))
		{
			bHasBlueprint = true;
			break;
		}
	}

	if (bHasBlueprint)
	{
		Extender->AddMenuExtension(
			"GetAssetActions",
			EExtensionHook::After,
			nullptr,
			FMenuExtensionDelegate::CreateLambda([this, SelectedAssets](FMenuBuilder& MenuBuilder)
				{
					MenuBuilder.AddMenuEntry(
						LOCTEXT("UncookBlueprint", "Uncook Blueprint"),
						LOCTEXT("UncookBlueprintTooltip", "Reconstruct the Blueprint graph from cooked bytecode. Creates a copy in an Uncooked/ subfolder with the original asset name (no _C suffix). Swap on disk with the cooked .uasset/.uexp to test in PIE."),
						FSlateIcon(FEditorStyle::GetStyleSetName(), "ClassIcon.Blueprint"),
						FUIAction(
							FExecuteAction::CreateRaw(this, &FBlueprintUncookerModule::ExecuteUncookBlueprint, TArray<FAssetData>(SelectedAssets))
						)
					);
					MenuBuilder.AddMenuEntry(
						LOCTEXT("LiveUncookBlueprint", "Uncook as Child Class (PIE Test)"),
						LOCTEXT("LiveUncookTooltip", "Creates a child Blueprint that inherits from the cooked class and overrides all logic with decompiled code. Casts to the original class work through inheritance. Set your GameMode to use the child class, then PIE to test."),
						FSlateIcon(FEditorStyle::GetStyleSetName(), "ClassIcon.Blueprint"),
						FUIAction(
							FExecuteAction::CreateRaw(this, &FBlueprintUncookerModule::ExecuteLiveUncook, TArray<FAssetData>(SelectedAssets))
						)
					);
				})
		);
	}

	return Extender;
}

void FBlueprintUncookerModule::ExecuteUncookBlueprint(TArray<FAssetData> SelectedAssets)
{
	int32 SuccessCount = 0;
	int32 FailCount = 0;

	FScopedSlowTask SlowTask(SelectedAssets.Num(), LOCTEXT("UncookingBlueprints", "Uncooking Blueprints..."));
	SlowTask.MakeDialog(true);

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (SlowTask.ShouldCancel())
		{
			break;
		}
		SlowTask.EnterProgressFrame(1.f, FText::FromString(FString::Printf(TEXT("Uncooking: %s"), *AssetData.AssetName.ToString())));

		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			UE_LOG(LogBlueprintUncooker, Warning, TEXT("Failed to load asset: %s"), *AssetData.ObjectPath.ToString());
			FailCount++;
			continue;
		}

		/* Get the BlueprintGeneratedClass - this is where the bytecode lives.
		   For cooked assets: the asset IS the BPGC directly (no UBlueprint wrapper).
		   For editor assets: the asset is a UBlueprint with a GeneratedClass member. */
		UBlueprintGeneratedClass* GeneratedClass = nullptr;

		// Try 1: Direct cast - cooked assets are UBlueprintGeneratedClass
		GeneratedClass = Cast<UBlueprintGeneratedClass>(Asset);

		// Try 2: It's a UBlueprint (editor asset) - get its generated class
		if (!GeneratedClass)
		{
			UBlueprint* SourceBP = Cast<UBlueprint>(Asset);
			if (SourceBP)
			{
				GeneratedClass = Cast<UBlueprintGeneratedClass>(SourceBP->GeneratedClass);
				if (!GeneratedClass)
				{
					GeneratedClass = Cast<UBlueprintGeneratedClass>(SourceBP->SkeletonGeneratedClass);
				}
			}
		}

		// Try 3: It might be some other UClass that has bytecode
		if (!GeneratedClass)
		{
			GeneratedClass = Cast<UBlueprintGeneratedClass>(Cast<UClass>(Asset));
		}

		if (!GeneratedClass)
		{
			UE_LOG(LogBlueprintUncooker, Warning, TEXT("Could not find BlueprintGeneratedClass for: %s (asset class: %s)"),
				*AssetData.AssetName.ToString(), *Asset->GetClass()->GetName());
			FailCount++;
			continue;
		}

		UE_LOG(LogBlueprintUncooker, Log, TEXT("Found generated class: %s (from asset: %s)"),
			*GeneratedClass->GetName(), *Asset->GetClass()->GetName());

		// Count functions with bytecode
		int32 FunctionCount = 0;
		int32 TotalBytecodeSize = 0;
		for (TFieldIterator<UFunction> FuncIt(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			FunctionCount++;
			TotalBytecodeSize += FuncIt->Script.Num();
		}

		UE_LOG(LogBlueprintUncooker, Log, TEXT("=== Uncooking: %s ==="), *GeneratedClass->GetName());
		UE_LOG(LogBlueprintUncooker, Log, TEXT("  Parent: %s"), *GeneratedClass->GetSuperClass()->GetName());
		UE_LOG(LogBlueprintUncooker, Log, TEXT("  Functions: %d (total bytecode: %d bytes)"), FunctionCount, TotalBytecodeSize);

		// Phase 1: Read bytecode into IR
		FBytecodeReader Reader;
		TArray<FDecompiledFunction> DecompiledFunctions = Reader.DecompileClass(GeneratedClass);

		if (Reader.GetErrors().Num() > 0)
		{
			UE_LOG(LogBlueprintUncooker, Warning, TEXT("%d errors during bytecode reading:"), Reader.GetErrors().Num());
			for (const FString& Error : Reader.GetErrors())
			{
				UE_LOG(LogBlueprintUncooker, Warning, TEXT("  %s"), *Error);
			}
		}

		UE_LOG(LogBlueprintUncooker, Log, TEXT("  Decompiled %d functions successfully"), DecompiledFunctions.Num());

		// Phase 2: Build the new Blueprint
		FString OriginalPath = AssetData.PackagePath.ToString();

		/* Strip the _C suffix from the generated class name to get the proper Blueprint
		   asset name. UE4 appends _C to compiled BP class names (e.g., Gregory_C -> Gregory). */
		FString BaseName = GeneratedClass->GetName();
		if (BaseName.EndsWith(TEXT("_C")))
		{
			BaseName.LeftChopInline(2);
		}

		/* Place the uncooked BP in an "Uncooked" subfolder with the original asset name
		   (no _C, no _Uncooked suffix). The user can then swap it into the original folder
		   so the class path matches what other BPs expect for casts. */
		FString NewBPName = BaseName;
		FString NewPackagePath = OriginalPath / TEXT("Uncooked") / NewBPName;

		UE_LOG(LogBlueprintUncooker, Log, TEXT("Output: %s (swap with cooked asset to test)"), *NewPackagePath);

		// Check if we already uncooked this before � clean up old version
		UPackage* ExistingPackage = FindPackage(nullptr, *NewPackagePath);
		if (ExistingPackage)
		{
			UE_LOG(LogBlueprintUncooker, Log, TEXT("Removing previous uncooked asset at %s"), *NewPackagePath);
			TArray<UObject*> ObjectsToDelete;
			ForEachObjectWithPackage(ExistingPackage, [&ObjectsToDelete](UObject* Object)
				{
					ObjectsToDelete.Add(Object);
					return true;
				});
			for (UObject* Obj : ObjectsToDelete)
			{
				Obj->MarkPendingKill();
			}
			ExistingPackage->MarkPendingKill();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		UPackage* NewPackage = CreatePackage(*NewPackagePath);
		if (!NewPackage)
		{
			UE_LOG(LogBlueprintUncooker, Error, TEXT("Failed to create package: %s"), *NewPackagePath);
			FailCount++;
			continue;
		}

		FGraphBuilder GraphBuilder;
		UBlueprint* NewBP = GraphBuilder.BuildBlueprint(GeneratedClass, DecompiledFunctions, NewPackage, NewBPName);

		if (!NewBP)
		{
			UE_LOG(LogBlueprintUncooker, Error, TEXT("Failed to build Blueprint graph"));
			FailCount++;
			continue;
		}

		if (GraphBuilder.GetWarnings().Num() > 0)
		{
			for (const FString& Warning : GraphBuilder.GetWarnings())
			{
				UE_LOG(LogBlueprintUncooker, Warning, TEXT("  %s"), *Warning);
			}
		}

		NewPackage->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(NewBP);

		/* Compile the Blueprint. GenerateBlueprintSkeleton + RefreshAllNodes already ran
		   inside BuildBlueprintCore, so the skeleton has function stubs and all node pins
		   are properly reconstructed before compilation. */
		FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

		// Post-compilation: copy CDO default values from the original class
		UBlueprintGeneratedClass* NewGenClass = Cast<UBlueprintGeneratedClass>(NewBP->GeneratedClass);
		if (NewGenClass)
		{
			UObject* OriginalCDO = GeneratedClass->GetDefaultObject(false);
			UObject* NewCDO = NewGenClass->GetDefaultObject(true);

			if (OriginalCDO && NewCDO)
			{
				// Copy all properties that exist on both classes
				int32 CopiedProps = 0;
				for (TFieldIterator<FProperty> PropIt(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
				{
					FProperty* OrigProp = *PropIt;
					FProperty* NewProp = NewGenClass->FindPropertyByName(OrigProp->GetFName());
					if (!NewProp) continue;

					// Skip component properties (handled separately below)
					if (OrigProp->IsA<FObjectPropertyBase>())
					{
						FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(OrigProp);
						if (ObjProp && ObjProp->PropertyClass &&
							ObjProp->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
						{
							continue;
						}
					}

					const void* SrcAddr = OrigProp->ContainerPtrToValuePtr<void>(OriginalCDO);
					void* DstAddr = NewProp->ContainerPtrToValuePtr<void>(NewCDO);

					OrigProp->CopyCompleteValue(DstAddr, SrcAddr);
					CopiedProps++;
				}

				UE_LOG(LogBlueprintUncooker, Log, TEXT("Copied %d CDO property defaults"), CopiedProps);
			}
		}

		// Post-compilation: copy component template property values by matching SCS nodes by name
		if (NewBP->SimpleConstructionScript && GeneratedClass)
		{
			UBlueprintGeneratedClass* OrigBPGC = Cast<UBlueprintGeneratedClass>(GeneratedClass);
			USimpleConstructionScript* OrigSCS = OrigBPGC ? OrigBPGC->SimpleConstructionScript : nullptr;

			if (OrigSCS)
			{
				const TArray<USCS_Node*>& NewNodes = NewBP->SimpleConstructionScript->GetAllNodes();
				const TArray<USCS_Node*>& OrigNodes = OrigSCS->GetAllNodes();

				for (USCS_Node* NewNode : NewNodes)
				{
					if (!NewNode || !NewNode->ComponentTemplate) continue;

					for (USCS_Node* OrigNode : OrigNodes)
					{
						if (!OrigNode || !OrigNode->ComponentTemplate) continue;

						if (OrigNode->GetVariableName() == NewNode->GetVariableName())
						{
							UActorComponent* SrcComp = OrigNode->ComponentTemplate;
							UActorComponent* DstComp = NewNode->ComponentTemplate;

							if (SrcComp->GetClass() == DstComp->GetClass())
							{
								for (TFieldIterator<FProperty> PropIt(SrcComp->GetClass()); PropIt; ++PropIt)
								{
									FProperty* Prop = *PropIt;
									const void* SrcAddr = Prop->ContainerPtrToValuePtr<void>(SrcComp);
									void* DstAddr = Prop->ContainerPtrToValuePtr<void>(DstComp);
									Prop->CopyCompleteValue(DstAddr, SrcAddr);
								}

								UE_LOG(LogBlueprintUncooker, Log, TEXT("Copied properties for component: %s"),
									*NewNode->GetVariableName().ToString());
							}
							break;
						}
					}
				}
			}

			// Also copy component template properties (from BPGC->ComponentTemplates)
			if (OrigBPGC)
			{
				for (int32 i = 0; i < OrigBPGC->ComponentTemplates.Num() && i < NewBP->ComponentTemplates.Num(); i++)
				{
					UActorComponent* SrcComp = OrigBPGC->ComponentTemplates[i];
					UActorComponent* DstComp = NewBP->ComponentTemplates[i];
					if (!SrcComp || !DstComp) continue;
					if (SrcComp->GetClass() != DstComp->GetClass()) continue;

					for (TFieldIterator<FProperty> PropIt(SrcComp->GetClass()); PropIt; ++PropIt)
					{
						FProperty* Prop = *PropIt;
						const void* SrcAddr = Prop->ContainerPtrToValuePtr<void>(SrcComp);
						void* DstAddr = Prop->ContainerPtrToValuePtr<void>(DstComp);
						Prop->CopyCompleteValue(DstAddr, SrcAddr);
					}
				}
			}
		}

		/* Post-compilation: copy inherited component overrides from CDO subobjects.
		   When the parent is a C++ class, components created by its constructor (e.g.,
		   SkeletalMeshComponent on ACharacter) are NOT in the SCS. Their property overrides
		   live on the CDO's subobjects and aren't captured by SCS node matching above. */
		if (NewGenClass)
		{
			UObject* OrigCDOForComponents = GeneratedClass->GetDefaultObject(false);
			UObject* NewCDOForComponents = NewGenClass->GetDefaultObject(true);

			if (OrigCDOForComponents && NewCDOForComponents)
			{
				// Collect component names handled by the SCS copy above so we skip them
				TSet<FName> SCSComponentNames;
				if (NewBP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : NewBP->SimpleConstructionScript->GetAllNodes())
					{
						if (Node && Node->ComponentTemplate)
						{
							SCSComponentNames.Add(Node->ComponentTemplate->GetFName());
						}
					}
				}

				TArray<UObject*> OrigCDOSubobjects;
				TArray<UObject*> NewCDOSubobjects;
				GetObjectsWithOuter(OrigCDOForComponents, OrigCDOSubobjects, /*bIncludeNestedObjects=*/ false);
				GetObjectsWithOuter(NewCDOForComponents, NewCDOSubobjects, /*bIncludeNestedObjects=*/ false);

				int32 InheritedCopyCount = 0;
				for (UObject* OrigSubobj : OrigCDOSubobjects)
				{
					UActorComponent* OrigComp = Cast<UActorComponent>(OrigSubobj);
					if (!OrigComp) continue;

					// Skip components already handled by SCS matching above
					if (SCSComponentNames.Contains(OrigComp->GetFName())) continue;

					UActorComponent* NewComp = nullptr;
					for (UObject* NewSubobj : NewCDOSubobjects)
					{
						if (NewSubobj->GetFName() == OrigSubobj->GetFName())
						{
							NewComp = Cast<UActorComponent>(NewSubobj);
							break;
						}
					}

					if (!NewComp || NewComp->GetClass() != OrigComp->GetClass()) continue;

					for (TFieldIterator<FProperty> PropIt(OrigComp->GetClass()); PropIt; ++PropIt)
					{
						FProperty* Prop = *PropIt;
						const void* SrcAddr = Prop->ContainerPtrToValuePtr<void>(OrigComp);
						void* DstAddr = Prop->ContainerPtrToValuePtr<void>(NewComp);
						Prop->CopyCompleteValue(DstAddr, SrcAddr);
					}

					InheritedCopyCount++;
					UE_LOG(LogBlueprintUncooker, Log, TEXT("Copied inherited component overrides: %s (%s)"),
						*OrigComp->GetFName().ToString(), *OrigComp->GetClass()->GetName());
				}

				if (InheritedCopyCount > 0)
				{
					UE_LOG(LogBlueprintUncooker, Log, TEXT("Copied %d inherited component override(s) from CDO"), InheritedCopyCount);
				}
			}
		}

		/* Do NOT recompile here - it would wipe the CDO defaults and component properties.
		   The first compile already generated the class structure, and we've now populated
		   the defaults. The user can recompile in the editor if needed. */
		NewPackage->MarkPackageDirty();

		int32 NumEventGraphNodes = 0;
		int32 NumFunctionGraphs = 0;
		for (UEdGraph* Graph : NewBP->UbergraphPages)
		{
			NumEventGraphNodes += Graph->Nodes.Num();
		}
		NumFunctionGraphs = NewBP->FunctionGraphs.Num();

		UE_LOG(LogBlueprintUncooker, Log, TEXT("=== SUCCESS: Created %s ==="), *NewBPName);
		UE_LOG(LogBlueprintUncooker, Log, TEXT("  Event graph nodes: %d"), NumEventGraphNodes);
		UE_LOG(LogBlueprintUncooker, Log, TEXT("  Function graphs: %d"), NumFunctionGraphs);

		{
			FGraphValidator Validator;
			Validator.DumpBlueprint(NewBP);
		}

		SuccessCount++;
	}

	FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("Blueprint Uncooker: %d succeeded, %d failed"), SuccessCount, FailCount)));
	Info.ExpireDuration = 5.0f;
	Info.bUseLargeFont = false;
	Info.bUseSuccessFailIcons = true;
	FSlateNotificationManager::Get().AddNotification(Info);
}

/* Live Uncook - Child Class Approach
   Creates a child Blueprint that inherits from the cooked class so Cast<OriginalClass>
   succeeds on child instances. All decompiled logic goes into the child as function/event
   overrides. Variables, components, and CDO defaults are inherited, not duplicated.
   To test: set GameMode's class to the child, then PIE. */

void FBlueprintUncookerModule::ExecuteLiveUncook(TArray<FAssetData> SelectedAssets)
{
	int32 SuccessCount = 0;
	int32 FailCount = 0;

	FScopedSlowTask SlowTask(SelectedAssets.Num(), LOCTEXT("LiveUncookingBlueprints", "Creating child class Blueprints..."));
	SlowTask.MakeDialog(true);

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (SlowTask.ShouldCancel()) break;
		SlowTask.EnterProgressFrame(1.f, FText::FromString(FString::Printf(TEXT("Uncooking as child: %s"), *AssetData.AssetName.ToString())));

		// Step 1: Resolve the cooked BlueprintGeneratedClass
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			UE_LOG(LogBlueprintUncooker, Warning, TEXT("ChildUncook: Failed to load asset: %s"), *AssetData.ObjectPath.ToString());
			FailCount++;
			continue;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Asset);
		if (!BPGC)
		{
			UBlueprint* SourceBP = Cast<UBlueprint>(Asset);
			if (SourceBP)
			{
				BPGC = Cast<UBlueprintGeneratedClass>(SourceBP->GeneratedClass);
				if (!BPGC)
					BPGC = Cast<UBlueprintGeneratedClass>(SourceBP->SkeletonGeneratedClass);
			}
		}
		if (!BPGC)
		{
			BPGC = Cast<UBlueprintGeneratedClass>(Cast<UClass>(Asset));
		}
		if (!BPGC)
		{
			UE_LOG(LogBlueprintUncooker, Warning, TEXT("ChildUncook: Could not find BPGC for: %s"), *AssetData.AssetName.ToString());
			FailCount++;
			continue;
		}

		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: === Uncooking as child class: %s ==="), *BPGC->GetName());

		// Step 2: Decompile bytecode
		FBytecodeReader Reader;
		TArray<FDecompiledFunction> DecompiledFunctions = Reader.DecompileClass(BPGC);

		if (Reader.GetErrors().Num() > 0)
		{
			for (const FString& Error : Reader.GetErrors())
			{
				UE_LOG(LogBlueprintUncooker, Warning, TEXT("ChildUncook: Bytecode error: %s"), *Error);
			}
		}
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Decompiled %d functions"), DecompiledFunctions.Num());

		// Step 3: Build child Blueprint
		FString OrigBPName = BPGC->GetName();
		if (OrigBPName.EndsWith(TEXT("_C")))
		{
			OrigBPName.LeftChopInline(2);
		}

		FString OriginalPath = AssetData.PackagePath.ToString();
		FString ChildBPName = OrigBPName + TEXT("_Uncook");
		FString ChildPkgPath = OriginalPath / TEXT("Uncooked") / ChildBPName;

		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Creating child: %s (parent: %s)"), *ChildPkgPath, *BPGC->GetName());

		// Clean up any previous child uncook package
		UPackage* ExistingPkg = FindPackage(nullptr, *ChildPkgPath);
		if (ExistingPkg)
		{
			TArray<UObject*> ObjectsToDelete;
			ForEachObjectWithPackage(ExistingPkg, [&ObjectsToDelete](UObject* Object)
				{
					ObjectsToDelete.Add(Object);
					return true;
				});
			for (UObject* Obj : ObjectsToDelete)
			{
				Obj->MarkPendingKill();
			}
			ExistingPkg->MarkPendingKill();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		UPackage* ChildPkg = CreatePackage(*ChildPkgPath);
		if (!ChildPkg)
		{
			UE_LOG(LogBlueprintUncooker, Error, TEXT("ChildUncook: Failed to create package: %s"), *ChildPkgPath);
			FailCount++;
			continue;
		}

		FGraphBuilder GraphBuilder;
		UBlueprint* ChildBP = GraphBuilder.BuildChildBlueprint(BPGC, DecompiledFunctions, ChildPkg, ChildBPName);

		if (!ChildBP)
		{
			UE_LOG(LogBlueprintUncooker, Error, TEXT("ChildUncook: BuildChildBlueprint failed for %s"), *BPGC->GetName());
			FailCount++;
			continue;
		}

		if (GraphBuilder.GetWarnings().Num() > 0)
		{
			for (const FString& Warning : GraphBuilder.GetWarnings())
			{
				UE_LOG(LogBlueprintUncooker, Warning, TEXT("ChildUncook: %s"), *Warning);
			}
		}

		// Step 4: Compile the child Blueprint
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Compiling child Blueprint..."));

		ChildPkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(ChildBP);

		FKismetEditorUtilities::CompileBlueprint(ChildBP, EBlueprintCompileOptions::SkipGarbageCollection);

		UBlueprintGeneratedClass* ChildBPGC = Cast<UBlueprintGeneratedClass>(ChildBP->GeneratedClass);
		if (!ChildBPGC)
		{
			UE_LOG(LogBlueprintUncooker, Error, TEXT("ChildUncook: Compilation produced no GeneratedClass"));
			FailCount++;
			continue;
		}

		// Keep the child BP alive so GC doesn't collect it
		LiveUncookedBPs.Add(ChildBP);

		// Done - log results and instructions
		int32 NumNodes = 0;
		for (UEdGraph* Graph : ChildBP->UbergraphPages)
		{
			NumNodes += Graph->Nodes.Num();
		}

		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: === SUCCESS: Created child class %s ==="), *ChildBPGC->GetName());
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Parent: %s (%p)"), *BPGC->GetName(), (void*)BPGC);
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Child:  %s (%p)"), *ChildBPGC->GetName(), (void*)ChildBPGC);
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Event graph nodes: %d, Function graphs: %d"), NumNodes, ChildBP->FunctionGraphs.Num());
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: Cast<%s> will succeed on child instances (inheritance)."), *BPGC->GetName());
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook:"));
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: TO TEST IN PIE:"));
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: 1. Set your GameMode to use '%s' instead of '%s'"), *ChildBPGC->GetName(), *BPGC->GetName());
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: 2. Enter PIE � the child class runs the decompiled logic"));
		UE_LOG(LogBlueprintUncooker, Log, TEXT("ChildUncook: 3. Casts to %s from other BPs will work (child IS-A parent)"), *BPGC->GetName());

		{
			FGraphValidator Validator;
			Validator.DumpBlueprint(ChildBP);
		}

		SuccessCount++;
	}

	FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("Child Uncook: %d succeeded, %d failed"), SuccessCount, FailCount)));
	Info.ExpireDuration = 5.0f;
	Info.bUseLargeFont = false;
	Info.bUseSuccessFailIcons = true;
	FSlateNotificationManager::Get().AddNotification(Info);
}

#undef LOCTEXT_NAMESPACE