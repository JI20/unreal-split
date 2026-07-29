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
				// IPlatformInputDeviceMapper, for pairing local co-op players with input devices.
				"ApplicationCore",
				// FUniqueNetIdWrapper, the backend-agnostic player id logged during registration.
				"CoreOnline",
				// UGameMapsSettings, for the entry-map fallback when a session ends.
				"EngineSettings",
				"NetCore",
				"OnlineSubsystem",
				"OnlineSubsystemUtils"
			}
			);
	}
}
