using UnrealBuildTool;

public class fnaf9EditorTarget : TargetRules {
	public fnaf9EditorTarget(TargetInfo Target) : base(Target) {
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V2;
		ExtraModuleNames.AddRange(new string[] {
			"fnaf9",
			"RandomItemSystem",
			"RoomSystem",
			"BlueprintUncooker",
		});
	}
}
