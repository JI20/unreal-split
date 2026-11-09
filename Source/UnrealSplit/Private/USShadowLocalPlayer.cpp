// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "USShadowLocalPlayer.h"

#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"

#include "UnrealSplitModule.h"

bool UUSShadowLocalPlayer::IsShadow(const ULocalPlayer* Player)
{
	return Player != nullptr && Player->IsA<UUSShadowLocalPlayer>();
}

bool UUSShadowLocalPlayer::IsShadowController(const APlayerController* Controller)
{
	return Controller != nullptr && IsShadow(Cast<ULocalPlayer>(Controller->Player));
}

void UUSShadowLocalPlayer::GetRealLocalPlayers(const UGameInstance* GameInstance, TArray<ULocalPlayer*>& OutPlayers)
{
	OutPlayers.Reset();

	if (GameInstance == nullptr)
	{
		return;
	}

	for (ULocalPlayer* const Player : GameInstance->GetLocalPlayers())
	{
		if (Player != nullptr && !IsShadow(Player))
		{
			OutPlayers.Add(Player);
		}
	}
}

int32 UUSShadowLocalPlayer::CountRealLocalPlayers(const UGameInstance* GameInstance)
{
	if (GameInstance == nullptr)
	{
		return 0;
	}

	int32 Count = 0;
	for (const ULocalPlayer* const Player : GameInstance->GetLocalPlayers())
	{
		if (Player != nullptr && !IsShadow(Player))
		{
			++Count;
		}
	}

	return Count;
}

ULocalPlayer* UUSShadowLocalPlayer::GetRealLocalPlayerByIndex(const UGameInstance* GameInstance, int32 Index)
{
	if (GameInstance == nullptr || Index < 0)
	{
		return nullptr;
	}

	int32 RealIndex = 0;
	for (ULocalPlayer* const Player : GameInstance->GetLocalPlayers())
	{
		if (Player == nullptr || IsShadow(Player))
		{
			continue;
		}

		if (RealIndex == Index)
		{
			return Player;
		}

		++RealIndex;
	}

	return nullptr;
}

int32 UUSShadowLocalPlayer::GetRealLocalPlayerIndex(const UGameInstance* GameInstance, const ULocalPlayer* Player)
{
	if (GameInstance == nullptr || Player == nullptr || IsShadow(Player))
	{
		return INDEX_NONE;
	}

	int32 RealIndex = 0;
	for (const ULocalPlayer* const Candidate : GameInstance->GetLocalPlayers())
	{
		if (Candidate == nullptr || IsShadow(Candidate))
		{
			continue;
		}

		if (Candidate == Player)
		{
			return RealIndex;
		}

		++RealIndex;
	}

	return INDEX_NONE;
}

APlayerController* UUSShadowLocalPlayer::GetFirstRealLocalPlayerController(const UGameInstance* GameInstance)
{
	const ULocalPlayer* const Player = GetRealLocalPlayerByIndex(GameInstance, 0);
	return Player != nullptr ? Player->PlayerController : nullptr;
}

void UUSShadowLocalPlayer::SendSplitJoin(TArray<FString>& Options)
{
	// Refused, not forwarded. See the class comment: a split join for this player
	// would ask the server for a connection it must never have, and a server with
	// no free connection answers by dropping the whole client.
	UE_LOG(LogUnrealSplit, Warning, TEXT("Refused a split join for shadow local player '%s'. A remote-view player never joins the session; if this fired, something asked the engine to add it as a real player."), *GetName());
}

void UUSShadowLocalPlayer::SendSplitJoin(TArray<FString>& Options, UNetDriver* NetDriver, UE::Net::EJoinFlags Flags)
{
	UE_LOG(LogUnrealSplit, Warning, TEXT("Refused a split join for shadow local player '%s' (net driver '%s')."), *GetName(), *GetNameSafe(NetDriver));
}

bool UUSShadowLocalPlayer::SpawnPlayActor(const FString& URL, FString& OutError, UWorld* InWorld)
{
	// True with nothing spawned, which is a lie the caller needs: UEngine::LoadMap
	// treats a false here as a fatal error, so refusing honestly would take the
	// process down. Reporting success and doing nothing leaves this player with no
	// controller — which is exactly what it should have, since travel tears the
	// shadow view down and the subsystem rebuilds it afterwards.
	UE_LOG(LogUnrealSplit, Warning, TEXT("Refused to spawn a play actor for shadow local player '%s'. It should have been torn down before this point; the remote view will be missing until it is rebuilt."), *GetName());
	return true;
}
