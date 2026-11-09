// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class APlayerController;

/** Log category for everything inside the UnrealSplit plugin. */
DECLARE_LOG_CATEGORY_EXTERN(LogUnrealSplit, Log, All);

/**
 * Runtime module of the UnrealSplit plugin.
 * UnrealSplit owns multiplayer session handling, screen layout and camera
 * perspective for two-player co-op. It never depends on game code.
 */
class FUnrealSplitModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

namespace UnrealSplit
{
	/**
	 * Everything the plugin has to redo for one player a seamless travel carried.
	 *
	 * Camera streams and session registration are both driven from the post-login
	 * event, and the engine broadcasts that from AGameModeBase::PostLogin alone. A
	 * carried player never logs in again, so without this the new world has no
	 * camera streams: both machines come up showing only their own view, with no
	 * partner stream, no split, and nothing in the log to say why.
	 *
	 * Per player, and deliberately not a sweep of the world at the end of the
	 * travel: the two players are not re-attached together. The host is swapped
	 * inside PostSeamlessTravel; a remote client is swapped only once it reports
	 * the new world loaded, hundreds of milliseconds later. A sweep at either
	 * moment serves the other player's dead controller.
	 *
	 * There is no engine event for this, so the game mode has to make the call.
	 * Override AGameModeBase::HandleSeamlessTravelPlayer and call this after
	 * Super, which is where the controller reference has been updated to the live
	 * one. AUSGameModeBase already does; a game whose mode does not derive from it
	 * makes the one call itself.
	 */
	UNREALSPLIT_API void NotifySeamlessTravelPlayer(APlayerController* PlayerController);
}
