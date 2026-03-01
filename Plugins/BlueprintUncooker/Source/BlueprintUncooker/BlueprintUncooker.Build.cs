using UnrealBuildTool;

public class BlueprintUncooker : ModuleRules
{
    public BlueprintUncooker(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",
            "BlueprintGraph",
            "KismetCompiler",
            "Kismet",
            "ContentBrowser",
            "Slate",
            "SlateCore",
            "InputCore",
            "EditorStyle",
            "GraphEditor",
            "AssetTools",
            "AssetRegistry",
        });
    }
}
