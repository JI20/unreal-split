// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

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
				// IPlatformInputDeviceMapper, for pairing local co-op players with input devices.
				"ApplicationCore",
				"Slate",
				"SlateCore",
				// FUniqueNetIdWrapper, the backend-agnostic player id logged during registration.
				"CoreOnline",
				// UGameMapsSettings, for the entry-map fallback when a session ends.
				"EngineSettings",
				"NetCore",
				"OnlineSubsystem",
				"OnlineSubsystemUtils",
				// The backend chosen at process start, resolved before the engine read it.
				"UnrealSplitStartup"
			}
			);
	}
}
