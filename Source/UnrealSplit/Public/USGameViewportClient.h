// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "USViewTypes.h"
#include "USGameViewportClient.generated.h"

class APlayerController;
class APlayerState;
class ULocalPlayer;

/**
 * Viewport client for UnrealSplit — the only place that decides where each
 * local player's image goes.
 *
 * Engine splitscreen offers fixed layouts only (halves, thirds, quarters), so
 * the split ratio is taken over here: the screen is divided vertically at an
 * arbitrary, animated position instead. Set as GameViewportClientClassName in
 * the project's DefaultEngine.ini. The GameMapsSettings splitscreen entries no
 * longer determine the layout; they are left in place because the engine still
 * derives safe zones and subtitle placement from the splitscreen type.
 *
 * The replicated FUSViewIntent is the target; the blend toward it lives here and
 * is per-machine state, so nothing about a blend is sent over the network. Two
 * machines that received the same intent converge on the same layout regardless
 * of when each of them started animating.
 *
 * Nothing here branches on session mode, and nothing assumes a particular number
 * of local players: the layout is a function of how many players actually render
 * into this viewport. One — a machine whose partner's camera stream has not
 * started delivering yet, or one with the remote view switched off — is a
 * full-screen layout, not a broken split.
 *
 * Slot order is presentation, not identity. By default each view sits in the slot
 * the server assigned the player it shows, so every machine paints the same
 * left/right framing. Player1Share always means that player's width: when a
 * machine opts into "own" order and puts player 1 on the right, the ratio is
 * applied to the right slot rather than to the left edge of the screen.
 *
 * The audio listener policy lives here for the same reason the slot order does: it
 * is a local presentation choice about the views this class already owns. The
 * engine gives one listener per drawn view and no way to refuse one, so "one set of
 * ears" and "two" are both expressed by where those listeners sit.
 */
UCLASS(Config = Game)
class UNREALSPLIT_API UUSGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	//~ Begin UGameViewportClient interface
	virtual void Tick(float DeltaTime) override;
	virtual void LayoutPlayers() override;
	//~ End UGameViewportClient interface

	/**
	 * Local presentation: who sits on the left of this machine's split.
	 *
	 * Unreplicated on purpose — the intent still says "player 1's share", and each
	 * machine maps that share onto whichever of its slots currently holds player 1.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetViewOrderMode(EUSViewOrderMode NewMode);

	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	EUSViewOrderMode GetViewOrderMode() const { return ViewOrderMode; }

	/**
	 * Local presentation: whether the level is heard from one view or from both.
	 *
	 * Lives here because the listener topology follows the view topology exactly —
	 * this is the class that knows how many views there are and which of them is
	 * somebody else's camera. Unreplicated, like the view order.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetAudioListenerMode(EUSAudioListenerMode NewMode);

	/** Drops a runtime override and goes back to the configured setting, Auto included. */
	void ResetAudioListenerModeToConfig();

	/** The policy in force right now, with Auto already resolved to own or shared. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	EUSAudioListenerMode GetAudioListenerMode() const;

	/** True while a runtime call is holding the configured setting off. */
	bool IsAudioListenerModeOverridden() const { return AudioListenerModeOverride.IsSet(); }

	/** One line for US.ViewState: the mode, what it means here, and where it came from. */
	FString DescribeAudioListenerPolicy() const;

	/**
	 * What Auto means for a given topology, as a pure function so that both
	 * answers can be asserted without building either topology.
	 */
	static EUSAudioListenerMode ResolveAutoAudioListenerMode(bool bHasShadowView);

	/** True when one of this machine's views is a partner's camera rather than a player. */
	bool HasShadowView() const;

	/** Left-to-right slots as currently laid out, for diagnostics. */
	void GetLaidOutSlots(TArray<ULocalPlayer*>& OutSlots) const;

	/**
	 * Index in the laid-out slots of the view that represents player 1 — the player
	 * the authority put in slot 0. INDEX_NONE only when there are no slots at all;
	 * with nothing better to go on it answers 0, since local creation order is join
	 * order on the one machine that has no stream to read an assignment from.
	 */
	int32 FindPlayer1SlotIndex(const TArray<ULocalPlayer*>& Slots) const;

	/** The PlayerState of the player a view shows, or null when unknown. */
	static APlayerState* GetViewPlayerState(const ULocalPlayer* Player);

	/**
	 * The slot the authority gave the player a view shows, or INDEX_NONE when this
	 * machine cannot know it.
	 *
	 * Only a shadow view can answer, because the assignment travels on the camera
	 * stream it renders. That is enough for a two-view layout: the local player
	 * takes whichever slot the shadow did not.
	 */
	static int32 GetAssignedViewSlot(const ULocalPlayer* Player);

	/**
	 * Takes a new intent as the blend target. Called on every machine, from
	 * AUSViewState's OnRep via UUSViewSubsystem.
	 *
	 * The very first intent a machine sees is snapped rather than blended: for a
	 * client joining mid-session it is not a change, it is the layout everybody
	 * else is already looking at.
	 */
	void SetViewIntent(const FUSViewIntent& NewIntent);

	/**
	 * Drives the ratio directly, with no blend and no replication.
	 *
	 * Precedence: this cancels a blend in flight and keeps the ratio until the
	 * next replicated intent arrives, which takes control back. That makes it the
	 * path for gameplay that moves the ratio continuously — each machine derives
	 * the ratio from replicated game state and drives it locally, so a per-frame
	 * ratio costs no bandwidth at all.
	 */
	void SetSplitRatioLocal(float Player1Share);

	/**
	 * Forgets everything that belonged to the previous world.
	 *
	 * This object outlives level travel while the intent, the blend and the saved
	 * pre-merge cameras do not; clearing them means the next world's intent is
	 * applied as a starting state rather than blended into from a layout nobody
	 * is looking at any more.
	 */
	void ResetForNewWorld();

	/** The intent this machine is blending toward. */
	const FUSViewIntent& GetViewIntent() const { return TargetIntent; }

	/** Share of the width player 1 has on this machine right now, mid-blend included. */
	float GetCurrentSplitRatio() const { return CurrentShare; }

	/** Share this machine is heading for: 1.0 while merged, otherwise the intent's. */
	float GetTargetSplitRatio() const;

	/** 0..1 through the running blend; 1 when nothing is in flight. */
	float GetBlendProgress() const;

	/** True while a local ratio drive is holding off the replicated intent. */
	bool IsLocalSplitRatioDriveActive() const { return bLocalDriveActive; }

private:
	/** Moves CurrentShare along the eased curve toward the target. */
	void AdvanceBlend(float DeltaTime);

	/** Points every local view at the merged target, or puts them back. */
	void UpdateMergedViewTargets();

	/**
	 * Origin/size of one slot.
	 *
	 * Player1SlotIndex is which of the laid-out slots represents player 1: that
	 * slot gets CurrentShare of the width, and the left edge of the split is
	 * derived from whether player 1 is currently on the left or the right. Slot 0
	 * is never assumed to be player 1.
	 */
	void ComputeSlotRect(int32 SlotIndex, int32 NumSlots, int32 Player1SlotIndex, float& OutOriginX, float& OutSizeX) const;

	/**
	 * Local players that render into this viewport, left-to-right.
	 *
	 * Order is ViewOrderMode: Fixed puts each view in the slot the authority
	 * assigned the player it shows, so both machines frame the pair the same way;
	 * Own keeps engine creation order so this machine's own player stays left.
	 */
	void GatherSlots(TArray<ULocalPlayer*>& OutSlots) const;

	/**
	 * Puts every listener this class is responsible for where the current policy
	 * says it belongs, or releases the ones it holds.
	 *
	 * Only a second *real* local player is dealt with here, and only because it has
	 * to be: that controller is the game's own class, so the plugin cannot override
	 * its listener position and drives the engine's public override instead. A
	 * shadow view answers for itself inside AUSSpectatorPlayerController.
	 *
	 * Re-run every frame because the transform it copies is the primary's live
	 * camera, not a fixed point.
	 */
	void UpdateAudioListenerPolicy();

	/** Hands back every listener this policy took over, and forgets them. */
	void ReleaseHeldAudioListeners();

	/**
	 * Reflected so that garbage collection can see the merged view target inside
	 * it: a bare pointer here would be left dangling by an actor the game
	 * destroys, and this one is game-supplied by definition.
	 */
	UPROPERTY()
	FUSViewIntent TargetIntent;

	/**
	 * Left-to-right presentation. Default Fixed so both machines show the same
	 * framing; Own restores "my player on the left" for machines that want it.
	 */
	UPROPERTY(Config)
	EUSViewOrderMode ViewOrderMode = EUSViewOrderMode::Fixed;

	/**
	 * Configured listener policy. Auto by default, which reproduces the behaviour
	 * every session had before the setting existed.
	 */
	UPROPERTY(Config)
	EUSAudioListenerModeSetting AudioListenerMode = EUSAudioListenerModeSetting::Auto;

	/** Set only by SetAudioListenerMode; outranks the configured setting until reset. */
	TOptional<EUSAudioListenerMode> AudioListenerModeOverride;

	/**
	 * Controllers whose listener position this policy currently owns.
	 *
	 * Remembered so that releasing gives back only what was taken: game code is
	 * free to use the same engine override for a cutscene or a security camera, and
	 * clearing that would be this plugin reaching into game territory. Weak because
	 * a couch player can leave, and travel takes every controller with it.
	 */
	TArray<TWeakObjectPtr<APlayerController>> HeldAudioListeners;

	/** Blend bookkeeping. All of it local; none of it replicates. */
	float CurrentShare = 0.5f;
	float BlendFromShare = 0.5f;
	float BlendElapsed = 0.0f;
	float BlendDuration = 0.0f;
	bool bHasAppliedIntent = false;
	bool bLocalDriveActive = false;

	/** What a controller looked at before the merge, and what it was last given. */
	struct FUSMergedViewTargetRecord
	{
		TWeakObjectPtr<AActor> PreMergeTarget;
		TWeakObjectPtr<AActor> AppliedTarget;
	};

	/**
	 * Keyed weakly on purpose: controllers come and go with travel and with
	 * couch players joining, and a merge that outlives one of them must not keep
	 * it alive or mistake a recycled pointer for the same controller.
	 */
	TMap<TWeakObjectPtr<APlayerController>, FUSMergedViewTargetRecord> MergedViewTargetRecords;
};
