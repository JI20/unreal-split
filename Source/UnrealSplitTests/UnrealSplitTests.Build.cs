// Copyright (c) Jonas Itt. All Rights Reserved.

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
