#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UBlueprint;

class FBlueprintUncookerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Extends the content browser context menu for Blueprint assets */
	TSharedRef<FExtender> ExtendContentBrowserMenu(const TArray<FAssetData>& SelectedAssets);

	/** Executes the uncook action on selected blueprints (creates new asset on disk) */
	void ExecuteUncookBlueprint(TArray<FAssetData> SelectedAssets);

	/** Live uncook: compiles uncooked graphs into a temp class, then swaps the
	 *  compiled bytecode onto the original cooked class IN MEMORY.
	 *  Same UClass*UFunction*FProperty* pointers preserved — all casts and
	 *  references from other BPs remain valid. No files changed on disk.
	 *  Reverts on editor restart. */
	void ExecuteLiveUncook(TArray<FAssetData> SelectedAssets);

	FDelegateHandle ContentBrowserExtenderHandle;

	/** Keep transient UBlueprints alive so GC doesn't collect them mid-session.
	 *  Each live-uncooked BP goes here to prevent its graphs from being destroyed. */
	TArray<UBlueprint*> LiveUncookedBPs;
};