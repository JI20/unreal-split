// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "USGameModeBase.generated.h"

/**
 * Replication-ready game mode base for two-player co-op.
 *
 * Contributes no gameplay: it only defaults the game state and player state to
 * the UnrealSplit bases and logs joins and leaves, which is what makes a listen
 * server session readable in the log. Games are expected to subclass this rather
 * than use it directly — spawn rules and match flow are game concerns and must
 * not migrate into the plugin.
 */
UCLASS()
class UNREALSPLIT_API AUSGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AUSGameModeBase();

	//~ Begin AGameModeBase interface
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	//~ End AGameModeBase interface
};
