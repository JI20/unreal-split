// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "USViewTypes.h"
#include "USGameViewportClient.generated.h"

class APlayerController;
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
 * into this viewport. One (an online client, until the 1.6 shadow view lands) is
 * a full-screen layout, not a broken split.
 */
UCLASS()
class UNREALSPLIT_API UUSGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	//~ Begin UGameViewportClient interface
	virtual void Tick(float DeltaTime) override;
	virtual void LayoutPlayers() override;
	//~ End UGameViewportClient interface

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

	/** Origin/size of one slot, given how many slots this viewport has. */
	void ComputeSlotRect(int32 SlotIndex, int32 NumSlots, float& OutOriginX, float& OutSizeX) const;

	/** Local players that render into this viewport, slot order; slot 0 is player 1. */
	void GatherSlots(TArray<ULocalPlayer*>& OutSlots) const;

	/**
	 * Reflected so that garbage collection can see the merged view target inside
	 * it: a bare pointer here would be left dangling by an actor the game
	 * destroys, and this one is game-supplied by definition.
	 */
	UPROPERTY()
	FUSViewIntent TargetIntent;

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
