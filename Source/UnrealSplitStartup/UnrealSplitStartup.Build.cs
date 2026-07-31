// Copyright (c) Jonas Itt. All Rights Reserved.

using UnrealBuildTool;

public class UnrealSplitStartup : ModuleRules
{
	public UnrealSplitStartup(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Core and nothing else, deliberately: this module is loaded before the
		// engine exists, and every module named here would be pulled into that
		// phase along with it.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
			);
	}
}
