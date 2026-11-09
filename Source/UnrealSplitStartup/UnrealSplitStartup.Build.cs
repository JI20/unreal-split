// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

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
