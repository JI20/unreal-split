// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "USViewTypes.generated.h"

/**
 * Camera perspective the players view and move the world through (axis 3).
 *
 * This is gameplay state, not just a camera choice: SideScroller constrains
 * movement to a plane, so it must only ever be changed by the server.
 */
UENUM(BlueprintType)
enum class EUSPerspective : uint8
{
	/** Free-roaming 3D camera, unconstrained movement. */
	Free3D,

	/** Side-scrolling camera; movement is constrained to the camera plane. */
	SideScroller
};

/**
 * Determines which camera drives the image while both players share a merged view.
 */
UENUM(BlueprintType)
enum class EUSMergedCameraPolicy : uint8
{
	/** A level-authored camera takes over; player cameras are ignored. */
	LockedAuthored,

	/** The merged view follows one designated player. */
	FollowLeader
};

/**
 * The authoritative, shared view intent for a co-op session — the complete
 * answer to "what should both players be seeing right now".
 *
 * Replicated as a single struct on purpose: perspective, merge state, split
 * ratio and blend duration must arrive together, otherwise a client can
 * briefly blend into a merged side-scroller view the server never asked for.
 *
 * Both players are always driven by the same intent, so they can never disagree
 * about perspective or framing — online included, where each client renders the
 * remote player's view alongside its own rather than going fullscreen.
 *
 * Only the in-flight blend progress is local, per-machine state.
 */
USTRUCT(BlueprintType)
struct FUSViewIntent
{
	GENERATED_BODY()

	/** Perspective both players view and move in. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	EUSPerspective Perspective = EUSPerspective::Free3D;

	/** Share of the screen given to player 1 while split, in the range 0..1. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	float Player1Share = 0.5f;

	/** True while both players share a single camera instead of their own. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	bool bMerged = false;

	/** Which camera drives the image while bMerged is true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	EUSMergedCameraPolicy MergedCameraPolicy = EUSMergedCameraPolicy::LockedAuthored;

	/** Seconds to blend into this intent; 0 applies instantly. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	float BlendTime = 1.0f;
};
