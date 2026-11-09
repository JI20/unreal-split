// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class UnrealSplitTests : ModuleRules
{
	public UnrealSplitTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"OnlineSubsystem",
				"OnlineSubsystemUtils",
				"UnrealSplit"
			}
			);
	}
}
