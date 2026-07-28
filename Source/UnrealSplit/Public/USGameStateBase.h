// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "USGameStateBase.generated.h"

/**
 * Replicated per-match state shared by both players.
 *
 * Intentionally empty: it exists so session-wide state added later (co-op
 * progress, view authority) has a replicated home that predates the game module,
 * and so the plugin's game mode has a matching default.
 */
UCLASS()
class UNREALSPLIT_API AUSGameStateBase : public AGameStateBase
{
	GENERATED_BODY()
};
