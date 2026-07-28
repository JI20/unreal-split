// Copyright (c) ji20. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USViewTypes.h"
#include "USViewSubsystem.generated.h"

class AUSViewState;
class UGameInstance;
class UWorld;

/**
 * View Manager for UnrealSplit — the single entry point for changing what the
 * two players see.
 *
 * UnrealSplit describes a co-op session along three independent axes:
 *   1. Session mode      — local splitscreen vs. online (UUSSessionSubsystem).
 *   2. Screen layout     — split ratio vs. a single merged view.
 *   3. Camera perspective — free 3D vs. side-scroller.
 *
 * This subsystem owns axes 2 and 3. Both players are always driven by the same
 * server-authoritative FUSViewIntent held by AUSViewState, so they can never
 * disagree about perspective or framing — perspective especially, since
 * side-scroller constrains movement. Every call below is a request to the
 * authority; on a client they are ignored with a warning rather than applied
 * locally. Only the in-flight blend progress is per-machine state.
 *
 * Every machine always has two view slots — its local player, and a shadow view
 * of the remote player — so an online client is laid out exactly like couch
 * co-op and the intent applies verbatim in both. Nothing here may branch on
 * session mode.
 *
 * Game-instance scoped on purpose. It drives UUSGameViewportClient, which is
 * owned by the game instance and survives level travel — a world-scoped manager
 * would reset its bookkeeping on travel while the viewport client kept the old
 * ratio, leaving the two disagreeing about the truth. World-dependent work
 * (cameras, spawning AUSViewState) is re-applied on world change instead.
 *
 * Skeleton only: the functions below log their arguments and do nothing else.
 */
UCLASS()
class UNREALSPLIT_API UUSViewSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/**
	 * Sets the share of the screen given to player 1, blending from the current
	 * ratio. Server only. Has no visible effect on a single-viewport client.
	 * @param Player1Share Fraction of the screen for player 1, in the range 0..1.
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetSplitRatio(float Player1Share, float BlendTime = 1.0f);

	/**
	 * Returns both players to their own cameras at the given ratio. Server only.
	 * @param Player1Share Fraction of the screen for player 1, in the range 0..1.
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SplitViews(float Player1Share = 0.5f, float BlendTime = 1.0f);

	/**
	 * Collapses both players onto a single shared camera. Server only.
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 * @param Policy Which camera drives the merged view.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void MergeViews(float BlendTime = 1.0f, EUSMergedCameraPolicy Policy = EUSMergedCameraPolicy::LockedAuthored);

	/**
	 * Switches the camera perspective for both players. Server only, because
	 * SideScroller constrains movement.
	 * @param Perspective Target perspective.
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetCameraPerspective(EUSPerspective Perspective, float BlendTime = 1.0f);

	/** The replicated view state for the current world, or null before it exists. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	AUSViewState* GetViewState() const;

	/** Applies a replicated intent locally. Called by AUSViewState on replication. */
	void ApplyViewIntent(const FUSViewIntent& Intent);

private:
	/** Re-acquires world-scoped state (view state actor, player cameras) after travel. */
	void HandleGameInstanceWorldChanged(UGameInstance* OwningGameInstance, UWorld* OldWorld, UWorld* NewWorld);

	FDelegateHandle WorldChangedHandle;
};
