// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USGameModeBase.h"

#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

#include "USGameStateBase.h"
#include "USPlayerStateBase.h"
#include "UnrealSplitModule.h"

namespace
{
	FString DescribePlayer(const AController* Controller)
	{
		const APlayerState* PlayerState = Controller ? Controller->PlayerState : nullptr;
		if (PlayerState && !PlayerState->GetPlayerName().IsEmpty())
		{
			return PlayerState->GetPlayerName();
		}

		return Controller ? Controller->GetName() : TEXT("Unknown");
	}
}

AUSGameModeBase::AUSGameModeBase()
{
	GameStateClass = AUSGameStateBase::StaticClass();
	PlayerStateClass = AUSPlayerStateBase::StaticClass();
}

void AUSGameModeBase::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	UE_LOG(LogUnrealSplit, Log, TEXT("Player joined: '%s' (%d player(s) in session, local=%d)."),
		*DescribePlayer(NewPlayer),
		GetNumPlayers(),
		(NewPlayer && NewPlayer->IsLocalController()) ? 1 : 0);
}

void AUSGameModeBase::Logout(AController* Exiting)
{
	// Logged before Super, which is where the controller is removed from the count.
	UE_LOG(LogUnrealSplit, Log, TEXT("Player left: '%s' (%d player(s) in session before removal)."),
		*DescribePlayer(Exiting),
		GetNumPlayers());

	Super::Logout(Exiting);
}
