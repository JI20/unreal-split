// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "USPlayerStateBase.generated.h"

/**
 * Replicated per-player state that survives respawns and travel.
 *
 * Intentionally empty: it exists so per-player data the view layer will need
 * (which of the two co-op slots a player occupies, remote view rotation) has a
 * replicated home, and so the plugin's game mode has a matching default.
 */
UCLASS()
class UNREALSPLIT_API AUSPlayerStateBase : public APlayerState
{
	GENERATED_BODY()
};
