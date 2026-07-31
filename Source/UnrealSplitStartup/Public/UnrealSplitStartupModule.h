// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Log category for the startup module.
 *
 * Separate from LogUnrealSplit on purpose: that category lives in the plugin's
 * runtime module, which is not loaded yet at this phase.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogUnrealSplitStartup, Log, All);

/**
 * The backend this process runs on, resolved once before anything reads it.
 *
 * Free functions rather than a subsystem because the answer exists long before
 * UObjects do, and it never changes afterwards.
 */
namespace UnrealSplitStartup
{
	/**
	 * Online subsystem this process runs on: whatever `DefaultPlatformService`
	 * named once the launch flag had its say, or `Null` with the key unset, which
	 * is the engine's own fallback for it.
	 */
	UNREALSPLITSTARTUP_API FName GetBackendName();

	/**
	 * Whether that backend finds games on the local network rather than through a
	 * hosted service. The only thing the rest of the plugin needs to know about
	 * the backend: it decides `bIsLANMatch` on session settings and whether a
	 * login is required.
	 */
	UNREALSPLITSTARTUP_API bool IsLanBackend();
}

/**
 * Picks the process-wide online backend from `-ConnectionMode=` at startup.
 *
 * There is exactly one backend per process, and the engine resolves the whole
 * online layer through it: `FOnlineSubsystemModule` reads
 * `DefaultPlatformService` once and caches it, and `ULocalPlayer` mints its
 * players' net ids from that same default. Choosing it here rather than per call
 * is what makes a local player's id natively the type the active backend
 * expects, which is why no id-conversion layer exists anywhere in the plugin.
 *
 * The loading phase is load-bearing and cannot be relaxed to `PostConfigInit`:
 * the engine's own `OnlineSubsystem` plugin is itself `PostConfigInit`, and
 * within one phase plugins load engine-before-project in an order that differs
 * between cooked and uncooked builds, so a `PostConfigInit` module cannot win
 * that race. `EarliestPossible` is strictly earlier and `GConfig` is already
 * live there.
 *
 * Absent the flag this module changes nothing, so the default is whatever
 * `DefaultEngine.ini` says and there is no second place that has to agree with
 * it. Precedence ends up: ini default, then `-ini:` overrides, then this flag.
 */
class FUnrealSplitStartupModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	//~ End IModuleInterface
};
