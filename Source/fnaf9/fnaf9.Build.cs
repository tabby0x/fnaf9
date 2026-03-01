using UnrealBuildTool;

public class fnaf9 : ModuleRules {
    public fnaf9(ReadOnlyTargetRules Target) : base(Target) {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bLegacyPublicIncludePaths = false;
        ShadowVariableWarningLevel = WarningLevel.Warning;
        
        PublicDependencyModuleNames.AddRange(new string[] {
            "AIModule",
            "AkAudio",
            "Chaos",
            "Core",
            "CoreUObject",
            "DeveloperSettings",
            "Engine",
            "GameplayTags",
            "GameplayTasks",
            "InputCore",
            "MovieScene",
            "MovieSceneTracks",
            "NavigationSystem",
            "PhysicsCore",
            "PropertyPath",
            "RandomItemSystem",
            "RoomSystem",
            "Slate",
            "SlateCore",
            "UMG",
            "JsonUtilities",
            "Json",
        });
    }
}
