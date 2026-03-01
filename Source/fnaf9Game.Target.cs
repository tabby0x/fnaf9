using UnrealBuildTool;

public class fnaf9GameTarget : TargetRules {
	public fnaf9GameTarget(TargetInfo Target) : base(Target) {
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V2;
		ExtraModuleNames.AddRange(new string[] {
			"fnaf9",
			"RandomItemSystem",
			"RoomSystem",
		});
	}
}
