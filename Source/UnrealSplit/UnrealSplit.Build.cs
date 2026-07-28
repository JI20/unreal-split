// Copyright (c) Jonas Itt. All Rights Reserved.

using UnrealBuildTool;

public class UnrealSplit : ModuleRules
{
	public UnrealSplit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"EnhancedInput"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// UGameMapsSettings, for the entry-map fallback when a session ends.
				"EngineSettings",
				"NetCore",
				"OnlineSubsystem",
				"OnlineSubsystemUtils"
			}
			);
	}
}
