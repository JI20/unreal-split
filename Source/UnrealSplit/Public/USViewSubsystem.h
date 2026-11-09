// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USViewTypes.h"
#include "USViewSubsystem.generated.h"

class AActor;
class AUSViewState;
class UGameInstance;
class UUSGameViewportClient;
class UWorld;
struct FActorsInitializedParams;

/**
 * View Manager for UnrealSplit — the single entry point for changing how the
 * screen is divided.
 *
 * UnrealSplit describes a co-op session along two axes:
 *   1. Session mode  — local splitscreen vs. online (UUSSessionSubsystem).
 *   2. Screen layout — split ratio vs. one merged view (here).
 *
 * Camera perspective used to be a third axis and is not any more (scope decision
 * 2026-08-03): the plugin owns how the viewport is divided, the game owns what
 * the cameras look at. The one thing that crosses that line is the merged view
 * target, which the game supplies and this plugin only forwards.
 *
 * Both players are always driven by the same server-authoritative FUSViewIntent,
 * held by AUSViewState, so they can never disagree about framing. The calls that
 * change the intent are authority-only and refuse elsewhere; there is no client
 * request path. Only the blend toward the intent is per-machine state, which is
 * why SetSplitRatioLocal exists and is the one call every machine may make.
 *
 * Game-instance scoped on purpose. It drives UUSGameViewportClient, which is
 * owned by the game instance and survives level travel — a world-scoped manager
 * would reset its bookkeeping on travel while the viewport client kept the old
 * ratio, leaving the two disagreeing about the truth. World-dependent work
 * (spawning AUSViewState, clearing a blend that belonged to the old world) is
 * re-applied on world change instead.
 *
 * Nothing here branches on session mode: standalone, LAN and EOS run identical
 * layout code, and in standalone the authority simply happens to be the only
 * machine there is.
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
	 * Sets the share of the screen width given to player 1. Server only.
	 *
	 * Does not leave a merged view: the ratio is remembered and takes effect when
	 * something calls SplitViews. On a machine with a single view the intent still
	 * replicates and is still queryable, it just has nothing to divide yet.
	 *
	 * @param Player1Share Fraction of the width for player 1, in the range 0..1.
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 * @param Easing Curve the transition follows.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetSplitRatio(float Player1Share, float BlendTime = 1.0f, EUSViewEasing Easing = EUSViewEasing::EaseInOut);

	/**
	 * Returns both players to their own views at the given ratio. Server only.
	 * @param Player1Share Fraction of the width for player 1, in the range 0..1.
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SplitViews(float Player1Share = 0.5f, float BlendTime = 1.0f);

	/**
	 * Collapses both players onto one full-screen view. Server only.
	 *
	 * The view target is the game's to choose and the game's to make addressable:
	 * this sends the reference, and it resolves on a remote machine either because
	 * the actor replicates or because it was placed in the level, which makes it
	 * addressable by path name with no replication at all. A level-authored fixed
	 * camera, a replicated camera rig and a player's pawn are all fine; an actor
	 * spawned locally at runtime is not. An unresolved or absent target still
	 * merges the layout and leaves the cameras where they are.
	 *
	 * @param BlendTime Duration of the transition in seconds; 0 applies instantly.
	 * @param ViewTarget Actor whose view fills the screen.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void MergeViews(float BlendTime = 1.0f, AActor* ViewTarget = nullptr);

	/**
	 * Drives the ratio on this machine only: immediate, no blend, no replication,
	 * and callable from anywhere including a client.
	 *
	 * This is the path for gameplay that moves the ratio continuously — derive the
	 * ratio from replicated game state and call this every frame on every machine,
	 * and a per-frame ratio costs no bandwidth. Precedence: a local drive cancels
	 * a blend in flight and holds the ratio until the next replicated intent
	 * arrives, which takes control back.
	 *
	 * @param Player1Share Fraction of the width for player 1, in the range 0..1.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|View")
	void SetSplitRatioLocal(float Player1Share);

	/** The replicated view state for the current world, or null before it exists. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	AUSViewState* GetViewState() const;

	/** The intent this machine is blending toward. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	FUSViewIntent GetViewIntent() const;

	/** Share of the width player 1 has on this machine right now, mid-blend included. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	float GetCurrentSplitRatio() const;

	/** Share this machine is heading for: 1.0 while merged, otherwise the intent's. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	float GetTargetSplitRatio() const;

	/** 0..1 through the running blend; 1 when nothing is in flight. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	float GetBlendProgress() const;

	/** True while a local ratio drive is holding off the replicated intent. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	bool IsLocalSplitRatioDriveActive() const;

	/** True where layout changes are allowed: the listen server, or standalone. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	bool IsViewAuthority() const;

	/** Applies a replicated intent locally. Called by AUSViewState on replication. */
	void ApplyViewIntent(const FUSViewIntent& Intent);

private:
	/**
	 * Spawns the view state for a world if this machine is the authority.
	 *
	 * Spawned from here rather than from AUSGameModeBase for three reasons: the
	 * game is free to use any game mode, and one that did not derive from the
	 * plugin's base would leave the whole view system silently dead; this
	 * subsystem is already what consumes the actor, so creation and use stay in
	 * one place; and the session layer deliberately does not depend on the
	 * framework bases, which the view layer has no reason to change.
	 *
	 * The timing is safe: OnWorldInitializedActors fires after UEngine::LoadMap
	 * has set the game mode and opened the listen socket, so the net mode is
	 * already settled, and it fires for every world in every mode — standalone,
	 * LAN and EOS reach this identically, with no session-mode branch.
	 */
	AUSViewState* EnsureViewState(UWorld* World);

	/** Finds the world's view state, replicated ones included, and caches it. */
	AUSViewState* FindViewState(UWorld* World) const;

	/** The plugin's viewport client, or null when this process renders nothing. */
	UUSGameViewportClient* GetUSViewportClient() const;

	void HandleWorldInitializedActors(const FActorsInitializedParams& Params);
	void HandleGameInstanceWorldChanged(UGameInstance* OwningGameInstance, UWorld* OldWorld, UWorld* NewWorld);

	/** Cache only — the actor is owned by the world, and by replication on a client. */
	mutable TWeakObjectPtr<AUSViewState> CachedViewState;

	FDelegateHandle WorldChangedHandle;
	FDelegateHandle ActorsInitializedHandle;
};
