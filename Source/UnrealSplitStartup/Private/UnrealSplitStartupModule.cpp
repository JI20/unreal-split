// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "UnrealSplitStartupModule.h"

#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(LogUnrealSplitStartup);

namespace
{
	/** Where FOnlineSubsystemModule reads the process-wide backend, once, at PostConfigInit. */
	const TCHAR* OnlineSubsystemSection = TEXT("OnlineSubsystem");
	const TCHAR* DefaultPlatformServiceKey = TEXT("DefaultPlatformService");

	/** Where UEngine reads the net driver class each connection type uses. */
	const TCHAR* EngineSection = TEXT("/Script/Engine.Engine");
	const TCHAR* NetDriverDefinitionsKey = TEXT("NetDriverDefinitions");
	const TCHAR* GameNetDriverDefName = TEXT("GameNetDriver");

	/**
	 * The one place in the plugin that names a backend or a net driver: a launch
	 * flag has to resolve to something concrete, and a second mapping elsewhere is
	 * how the two drift apart. Both names double as accepted flag values.
	 */
	const TCHAR* LanBackendName = TEXT("Null");
	const TCHAR* InternetBackendName = TEXT("EOS");

	const TCHAR* LanNetDriverClass = TEXT("/Script/OnlineSubsystemUtils.IpNetDriver");
	const TCHAR* InternetNetDriverClass = TEXT("/Script/SocketSubsystemEOS.NetDriverEOS");

	/** Always the IP driver: a fallback only runs when the primary failed to load. */
	const TCHAR* NetDriverFallbackClass = TEXT("/Script/OnlineSubsystemUtils.IpNetDriver");

	/** Resolved in StartupModule and never written again. */
	FString GResolvedBackendName;

	bool IsGameNetDriverEntry(const FString& Definition)
	{
		// Quotes around struct members are optional in ini, so they are dropped
		// before matching rather than assumed.
		return Definition.Replace(TEXT("\""), TEXT("")).Contains(FString::Printf(TEXT("DefName=%s"), GameNetDriverDefName), ESearchCase::IgnoreCase);
	}

	/** The definition as configured, so the startup line says what travel will use. */
	FString GetGameNetDriverEntry()
	{
		TArray<FString> Definitions;
		GConfig->GetArray(EngineSection, NetDriverDefinitionsKey, Definitions, GEngineIni);

		const FString* Entry = Definitions.FindByPredicate(&IsGameNetDriverEntry);
		return Entry != nullptr ? *Entry : FString(TEXT("no GameNetDriver definition"));
	}

	void SetGameNetDriver(const TCHAR* DriverClass)
	{
		// Read-modify-write: BaseEngine.ini defines BeaconNetDriver and
		// DemoNetDriver in the same array, and a wholesale write would drop them.
		TArray<FString> Definitions;
		GConfig->GetArray(EngineSection, NetDriverDefinitionsKey, Definitions, GEngineIni);

		const FString GameNetDriver = FString::Printf(TEXT("(DefName=\"%s\",DriverClassName=\"%s\",DriverClassNameFallback=\"%s\")"),
			GameNetDriverDefName, DriverClass, NetDriverFallbackClass);

		if (FString* Existing = Definitions.FindByPredicate(&IsGameNetDriverEntry))
		{
			*Existing = GameNetDriver;
		}
		else
		{
			UE_LOG(LogUnrealSplitStartup, Warning, TEXT("No '%s' definition to rewrite, so one is being added. Something has replaced the engine's own net driver definitions."), GameNetDriverDefName);
			Definitions.Add(GameNetDriver);
		}

		GConfig->SetArray(EngineSection, NetDriverDefinitionsKey, Definitions, GEngineIni);
	}

	/** The one line that answers "which backend is this process on"; Display so it shows without -log. */
	void LogResolvedMode(const TCHAR* Source)
	{
	UE_LOG(LogUnrealSplitStartup, Display, TEXT("=== UnrealSplit connection mode: %s === %s, selected by %s."),
		*UnrealSplitStartup::GetBackendName().ToString(),
		*GetGameNetDriverEntry(),
		Source);
	}
}

FName UnrealSplitStartup::GetBackendName()
{
	// Null with nothing configured, because that is the engine's own fallback for
	// an unset DefaultPlatformService (FOnlineSubsystemModule::LoadDefaultSubsystem)
	// and callers should not have to special-case "no answer".
	return GResolvedBackendName.IsEmpty() ? FName(LanBackendName) : FName(*GResolvedBackendName);
}

bool UnrealSplitStartup::IsLanBackend()
{
	// An unset DefaultPlatformService is LAN too: that is the engine's own
	// fallback for it (FOnlineSubsystemModule::LoadDefaultSubsystem).
	return GResolvedBackendName.IsEmpty() || GResolvedBackendName.Equals(LanBackendName, ESearchCase::IgnoreCase);
}

void FUnrealSplitStartupModule::StartupModule()
{
	// Read first: with no flag this is the answer, and with one it is what the
	// flag overrides. Both `-ini:` overrides and the ini hierarchy are already
	// folded in, which is what puts the flag last in precedence.
	GConfig->GetString(OnlineSubsystemSection, DefaultPlatformServiceKey, GResolvedBackendName, GEngineIni);

	FString RequestedMode;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ConnectionMode="), RequestedMode))
	{
		LogResolvedMode(TEXT("config"));
		return;
	}

	// The flag names the backend it selects, so there is one vocabulary for the
	// launch argument, the config value it writes and the log that reports it. LAN
	// and Internet stay accepted as aliases. There is deliberately no value for
	// local co-op: it uses no online subsystem and no session, so it plays on
	// either backend and a third value would imply a choice that does not exist.
	const bool bIsLanRequested = RequestedMode.Equals(LanBackendName, ESearchCase::IgnoreCase)
		|| RequestedMode.Equals(TEXT("LAN"), ESearchCase::IgnoreCase);
	const bool bIsInternetRequested = RequestedMode.Equals(InternetBackendName, ESearchCase::IgnoreCase)
		|| RequestedMode.Equals(TEXT("Internet"), ESearchCase::IgnoreCase);

	if (!bIsLanRequested && !bIsInternetRequested)
	{
		UE_LOG(LogUnrealSplitStartup, Warning, TEXT("-ConnectionMode=%s is not a connection mode. Accepted values are %s (alias LAN) and %s (alias Internet), case-insensitive. Config is left untouched."),
			*RequestedMode, LanBackendName, InternetBackendName);
		LogResolvedMode(TEXT("config, after an unrecognised -ConnectionMode"));
		return;
	}

	GResolvedBackendName = bIsLanRequested ? LanBackendName : InternetBackendName;

	// Both writes are process-local and must stay that way. A config write marks
	// the branch dirty, and the flush at shutdown is then free to persist it into
	// Saved/Config, where a single launch flag would silently outlive the launch
	// that asked for it. Nothing has written this branch yet at this phase, so
	// restoring the flag cannot hide someone else's pending write.
	FConfigFile* EngineConfig = GConfig->FindConfigFile(GEngineIni);
	const bool bWasDirty = EngineConfig != nullptr && EngineConfig->Dirty;

	GConfig->SetString(OnlineSubsystemSection, DefaultPlatformServiceKey, *GResolvedBackendName, GEngineIni);

	// Set explicitly per mode rather than left to the engine's own
	// DriverClassNameFallback: with the EOS driver installed it can report itself
	// as available during a LAN game, and the fallback would then never run.
	SetGameNetDriver(bIsLanRequested ? LanNetDriverClass : InternetNetDriverClass);

	if (EngineConfig != nullptr)
	{
		EngineConfig->Dirty = bWasDirty;
	}

	LogResolvedMode(TEXT("-ConnectionMode"));
}

IMPLEMENT_MODULE(FUnrealSplitStartupModule, UnrealSplitStartup)
