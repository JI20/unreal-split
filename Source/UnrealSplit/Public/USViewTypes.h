// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "USViewTypes.generated.h"

class AActor;

/**
 * Shape of the curve a layout change follows on its way to the new intent.
 *
 * Purely presentational and purely local: the curve is evaluated per machine
 * from the replicated BlendTime, so it costs no bandwidth and two machines that
 * received the same intent land on the same layout even if they picked up the
 * blend at different moments.
 */
UENUM(BlueprintType)
enum class EUSViewEasing : uint8
{
	/** Constant rate. */
	Linear,

	/** Starts slow, arrives at full speed. */
	EaseIn,

	/** Starts at full speed, settles into the target. */
	EaseOut,

	/** Slow at both ends; the default, because both ends are visible here. */
	EaseInOut
};

/**
 * The authoritative, shared screen layout for a co-op session — the complete
 * answer to "how is the screen divided right now".
 *
 * Layout only. What the cameras look at is game territory (scope decision
 * 2026-08-03); the one exception is MergedViewTarget below, and only because
 * "merged" is meaningless without knowing what fills the merged slot.
 *
 * Replicated as a single struct on purpose: split ratio, merge state and the
 * merged view target must arrive together. Sent as separate properties they can
 * land in different frames, and a client would spend those frames merged onto a
 * null camera, or splitting at a ratio the server has already left.
 *
 * Both machines are always driven by the same intent, so they can never
 * disagree about framing. Only the in-flight blend progress is local state.
 */
USTRUCT(BlueprintType)
struct FUSViewIntent
{
	GENERATED_BODY()

	/** Share of the screen width given to player 1 while split, in the range 0..1. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	float Player1Share = 0.5f;

	/** True while both players share one full-screen view instead of their own. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	bool bMerged = false;

	/**
	 * Actor whose view fills the screen while bMerged is true.
	 *
	 * Game-supplied, and the game's responsibility to make addressable on remote
	 * machines. This replicates as an object reference, and there are two ways for
	 * one to resolve: a replicated actor gets a net id when it replicates, and an
	 * actor placed in the level is addressable by path name without replicating at
	 * all (a level-authored fixed camera is the second kind). An actor spawned
	 * locally at runtime is neither. A reference that cannot be resolved arrives
	 * as null, which the viewport client treats as "merge the layout, leave the
	 * cameras alone" and retries every frame, so an actor that becomes relevant
	 * late still takes over.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	TObjectPtr<AActor> MergedViewTarget = nullptr;

	/** Seconds to blend into this intent; 0 applies instantly. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	float BlendTime = 1.0f;

	/** Curve the blend follows. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	EUSViewEasing Easing = EUSViewEasing::EaseInOut;
};
