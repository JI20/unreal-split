// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "USViewTypes.h"
#include "USViewState.generated.h"

/**
 * Server-authoritative carrier for the shared screen layout.
 *
 * How the screen is divided is gameplay-adjacent state — a merged view or a
 * lopsided split is something puzzle logic decides for both players at once —
 * so the decision lives on the server and replicates down; clients only render
 * it. Spawned by UUSViewSubsystem on the authority once per world, and always
 * relevant so both players receive it regardless of where they stand.
 *
 * The setters below are authority-only. There is intentionally no client
 * request path: layout changes are triggered by level volumes and puzzle logic,
 * which already run on the server. A client could not Server-RPC through this
 * actor anyway, since it owns no connection — if player-initiated layout
 * requests are ever needed they must route through the owning PlayerController.
 */
UCLASS()
class UNREALSPLIT_API AUSViewState : public AInfo
{
	GENERATED_BODY()

public:
	AUSViewState();

	//~ Begin AActor interface
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	/** Current replicated view intent. Valid on both server and clients. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	FUSViewIntent GetViewIntent() const { return ViewIntent; }

	/**
	 * Sets the share of the screen given to player 1. Server only.
	 *
	 * Does not leave a merged view — the ratio is remembered and takes effect
	 * when something calls ClearMerged.
	 *
	 * @param Player1Share Fraction of the screen for player 1, in the range 0..1.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 * @param Easing Curve the blend follows.
	 */
	void SetSplitRatio(float Player1Share, float BlendTime, EUSViewEasing Easing);

	/**
	 * Collapses both players onto one full-screen view. Server only.
	 *
	 * @param ViewTarget Actor whose view fills the screen; must be a replicated
	 *        actor for remote machines to resolve it. May be null, which merges
	 *        the layout and leaves the cameras where they are.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 * @param Easing Curve the blend follows.
	 */
	void SetMerged(AActor* ViewTarget, float BlendTime, EUSViewEasing Easing);

	/**
	 * Returns both players to their own views at the given ratio. Server only.
	 * @param Player1Share Fraction of the screen for player 1, in the range 0..1.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 * @param Easing Curve the blend follows.
	 */
	void ClearMerged(float Player1Share, float BlendTime, EUSViewEasing Easing);

protected:
	/** Applies the newly replicated intent to this machine's view subsystem. */
	UFUNCTION()
	void OnRep_ViewIntent();

	UPROPERTY(ReplicatedUsing = OnRep_ViewIntent, BlueprintReadOnly, Category = "UnrealSplit|View")
	FUSViewIntent ViewIntent;

private:
	/**
	 * The single authority-side write path: stores the intent and then calls the
	 * OnRep by hand.
	 *
	 * The manual call is not belt-and-braces, it is the only reason the host ever
	 * animates. A RepNotify never fires on the machine that made the change, and
	 * in standalone or local co-op the authority is the *only* machine — leave it
	 * out and the intent replicates perfectly to nobody while the screen in front
	 * of the player never moves. Every mutation goes through here so that call
	 * cannot be forgotten in one of them.
	 */
	void SetViewIntentOnAuthority(const FUSViewIntent& NewIntent);
};
