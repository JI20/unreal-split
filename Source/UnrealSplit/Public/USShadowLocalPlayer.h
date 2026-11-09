// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/LocalPlayer.h"
#include "USShadowLocalPlayer.generated.h"

class APlayerController;
class UGameInstance;
class UNetDriver;
class UWorld;

/**
 * A local player that exists only so the engine will render a second view — the
 * window onto the remote partner's camera.
 *
 * It is a real ULocalPlayer, because that is the only thing the renderer will
 * give a viewport slot to, and that is precisely the danger: everything in the
 * engine and in this plugin that means "a player on this machine" will find it
 * unless it is told otherwise. Being its own class is what makes that possible —
 * every exclusion in the plugin asks IsShadow rather than carrying a flag around,
 * and a new piece of code that forgets to ask is a bug that can be found by
 * searching for the local-player accessors.
 *
 * Two engine paths would let it leak out of this machine, and both are shut here
 * rather than avoided by careful calling:
 *
 *   1. Split join. A second local player normally asks the server for a child
 *      connection — from UGameInstance::CreateLocalPlayer, and again for every
 *      extra local player from UNetConnection::HandleClientPlayer on the next
 *      connect or travel. The second one is not a call this plugin makes, which
 *      is why passing bSpawnPlayerController=false would not be enough. A server
 *      with no free connection responds to a split join by dropping the entire
 *      client, so the cost of missing one is the session. Both overloads are
 *      virtual, so overriding them here makes the request unsendable from any
 *      path, present or future.
 *
 *   2. Play actor spawning. UEngine::LoadMap spawns a play actor for every local
 *      player and treats failure as fatal, and on a listen server that spawn is
 *      a real login: a game-mode PostLogin, a player state, a session
 *      registration, a seat consumed. The shadow view is torn down before travel
 *      for exactly this reason; this override is what makes a travel path nobody
 *      anticipated degrade into a missing view rather than a phantom player.
 */
UCLASS()
class UNREALSPLIT_API UUSShadowLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	/** True for a local player that only exists to render somebody else's view. */
	static bool IsShadow(const ULocalPlayer* Player);

	/** True for the locally spawned controller that drives such a view. */
	static bool IsShadowController(const APlayerController* Controller);

	/** Local players that represent someone sitting at this machine. */
	static void GetRealLocalPlayers(const UGameInstance* GameInstance, TArray<ULocalPlayer*>& OutPlayers);

	/**
	 * How many people are actually playing on this machine.
	 *
	 * The number every capacity, seat and co-op decision means, as opposed to
	 * UGameInstance::GetNumLocalPlayers, which counts views.
	 */
	static int32 CountRealLocalPlayers(const UGameInstance* GameInstance);

	/** The nth real local player, so "player 1" and "player 2" keep meaning people. */
	static ULocalPlayer* GetRealLocalPlayerByIndex(const UGameInstance* GameInstance, int32 Index);

	/** Index of a real local player among the real ones, or INDEX_NONE for a shadow. */
	static int32 GetRealLocalPlayerIndex(const UGameInstance* GameInstance, const ULocalPlayer* Player);

	/** The first real local player's controller — the machine's own point of view. */
	static APlayerController* GetFirstRealLocalPlayerController(const UGameInstance* GameInstance);

	//~ Begin ULocalPlayer interface
	virtual void SendSplitJoin(TArray<FString>& Options) override;
	virtual void SendSplitJoin(TArray<FString>& Options, UNetDriver* NetDriver, UE::Net::EJoinFlags Flags) override;
	virtual bool SpawnPlayActor(const FString& URL, FString& OutError, UWorld* InWorld) override;
	//~ End ULocalPlayer interface
};
