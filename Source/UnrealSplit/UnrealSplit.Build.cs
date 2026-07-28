// Copyright (c) ji20. All Rights Reserved.

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
				"NetCore",
				"OnlineSubsystem",
				"OnlineSubsystemUtils"
			}
			);
	}
}
