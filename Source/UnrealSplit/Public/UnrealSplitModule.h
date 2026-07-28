// Copyright (c) ji20. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

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
