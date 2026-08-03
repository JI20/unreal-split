// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USRemoteViewSubsystem.generated.h"

class AController;
class AGameModeBase;
class APlayerController;
class AUSPlayerViewProxy;
class AUSSpectatorPlayerController;
class ULocalPlayer;
class UUSShadowLocalPlayer;
class UWorld;

/**
 * Puts the remote partner on this machine's screen.
 *
 * Online, each machine simulates the whole world but renders only its own view.
 * This fills the second slot with the partner's camera, so both machines show
 * both players — the same picture a couch session shows, arrived at differently.
 * It does that with two pieces, and owns the lifetime of both:
 *
 *   - AUSPlayerViewProxy, one per player, which carries that player's finished
 *     camera to every machine.
 *   - A shadow local player and a locally spawned spectator controller, which
 *     turn a received stream into a scene view the renderer will draw.
 *
 * Layout is not its business. The 1.5 slot maths is driven by how many local
 * views exist and by the viewport's view-order mode, so adding the shadow player
 * is the whole of what creates a second slot — which side it lands on is decided
 * by identity sort (Fixed) or creation order (Own), not here.
 *
 * Separate from UUSViewSubsystem on purpose. That subsystem answers "how is the
 * screen divided", which is replicated, authoritative and identical everywhere.
 * This one answers "does this machine have anything to put in the second slot",
 * which is a purely local question with a different answer on every machine and a
 * completely different lifetime. Mixing them would put local view plumbing inside
 * the one class that must stay a faithful mirror of replicated intent.
 *
 * Everything here is event-driven — no polling for a partner, no session-mode
 * branches. A stream that starts delivering creates the view; a stream that stops
 * existing, a travel, a second real player at this machine, or the toggle going
 * off take it away.
 */
UCLASS(Config = Game)
class UNREALSPLIT_API UUSRemoteViewSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/**
	 * Turns the remote view on or off on this machine only.
	 *
	 * Local and unreplicated by design: whether this machine draws the partner is
	 * a rendering choice, not shared state, and either machine may make it
	 * differently without the two disagreeing about anything that matters.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetRemoteViewEnabled(bool bEnabled);

	/** True while this machine is willing to draw the partner's view. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	bool IsRemoteViewEnabled() const { return bRemoteViewEnabled; }

	/** True while a second scene view is actually being drawn from a remote stream. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	bool IsShadowViewActive() const;

	/** The stream currently feeding the shadow view, for diagnostics. */
	AUSPlayerViewProxy* GetShadowViewSource() const;

	/** Why there is no shadow view right now, in one sentence, for US.ViewState. */
	FString DescribeShadowViewState() const;

	/** Called by a proxy once it has a sample to show. */
	void NotifyRemoteStreamLive(AUSPlayerViewProxy* Proxy);

	/** Called by a proxy as it goes away, for any reason. */
	void NotifyRemoteStreamLost(AUSPlayerViewProxy* Proxy);

private:
	/** This game instance's world, or null between worlds. */
	UWorld* GetOwningWorld() const;

	/** Decides whether a shadow view should exist right now, and makes that true. */
	void EvaluateShadowView();

	bool CreateShadowView(AUSPlayerViewProxy* Source);
	void DestroyShadowView(const TCHAR* Reason);

	/** A delivering stream that belongs to somebody else, or null. */
	AUSPlayerViewProxy* FindRemoteStream() const;

	/**
	 * A platform user id no physical device is expected to answer to.
	 *
	 * The shadow player must have one — UGameInstance::FindLocalPlayerFromDeviceId
	 * matches local players by platform user, and an invalid id would match every
	 * unmapped device, handing real input to the one player that must never get
	 * any. So it takes an id above the splitscreen cap instead, checking that no
	 * connected device currently maps to it. The engine documents this same
	 * technique in CreateLocalPlayer, where an id above the cap is warned about as
	 * "unlikely to map to any physical device".
	 */
	FPlatformUserId FindUnusedPlatformUserId() const;

	void HandleGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer);
	void HandleGameModeLogout(AGameModeBase* GameMode, AController* Exiting);
	void HandleLocalPlayerChanged(ULocalPlayer* Player);
	void HandleWorldBeginTearDown(UWorld* World);
	void HandleSeamlessTravelStart(UWorld* World, const FString& TravelUrl);

	/** Server side: gives a player that just logged in a stream of its own. */
	void EnsureViewProxyForPlayer(APlayerController* PlayerController);

	/**
	 * Server side: the view slot this player owns for the rest of the session.
	 *
	 * The whole point of deciding this here is that this object outlives worlds, so
	 * a slot survives level travel, and outlives a connection, so a player who
	 * leaves and comes back is put back on the side they were on. Both are things a
	 * receiver cannot work out for itself from replicated player state, whose
	 * numbering the engine is free to reassign on rejoin.
	 *
	 * Keyed on the online identity where there is one. Without one — standalone or
	 * a LAN session with no login — it degrades to join order, which is all the
	 * identity that exists in that case and is what the pre-1.6 code did anyway.
	 */
	int32 AssignViewSlot(const APlayerController* PlayerController);

	/** True while a live stream already sits in this slot. */
	bool IsViewSlotTaken(int32 SlotIndex) const;

	UPROPERTY(Transient)
	TObjectPtr<UUSShadowLocalPlayer> ShadowPlayer;

	UPROPERTY(Transient)
	TObjectPtr<AUSSpectatorPlayerController> ShadowController;

	UPROPERTY(Transient)
	TObjectPtr<AUSPlayerViewProxy> ShadowSource;

	/** Proxies this machine spawned as the authority, so logout can clean them up. */
	UPROPERTY(Transient)
	TMap<TObjectPtr<APlayerController>, TObjectPtr<AUSPlayerViewProxy>> ViewProxies;

	/**
	 * Authority side: who owns which view slot, indexed by slot.
	 *
	 * Never cleared on logout — holding a departed player's claim is exactly what
	 * lets a reconnect land back on the same side. A slot is only handed to someone
	 * else once nobody is streaming from it.
	 */
	TArray<FUniqueNetIdRepl> ViewSlotOwners;

	/**
	 * Guards the local-player events against the shadow player's own creation:
	 * adding it broadcasts the same event that asks whether it should exist.
	 */
	bool bMutatingShadowView = false;

	/** Last refusal reason, so US.ViewState can say why there is no second view. */
	FString LastShadowViewNote;

	/** Whether this machine draws the partner at all; the console command flips it. */
	UPROPERTY(Config)
	bool bRemoteViewEnabled = true;

	/**
	 * First platform user index to try for the shadow player. Above the two seats
	 * this game can ever use, and above the engine's splitscreen cap, so no
	 * physical device is mapped to it.
	 */
	UPROPERTY(Config)
	int32 ShadowPlatformUserIndex = 7;

	FDelegateHandle PostLoginHandle;
	FDelegateHandle LogoutHandle;
	FDelegateHandle LocalPlayerAddedHandle;
	FDelegateHandle LocalPlayerRemovedHandle;
	FDelegateHandle WorldTearDownHandle;
	FDelegateHandle SeamlessTravelHandle;
};
