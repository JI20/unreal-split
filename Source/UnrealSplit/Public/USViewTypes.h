// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
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
 * How this machine orders the views left-to-right.
 *
 * Presentation only: never replicated, never changes FUSViewIntent, and never
 * changes what Player1Share means. Player1Share is always "player 1's width",
 * and the viewport maps that width onto whichever slot currently holds player 1.
 */
UENUM(BlueprintType)
enum class EUSViewOrderMode : uint8
{
	/**
	 * Player identity. Lowest APlayerState::PlayerId on the left on every machine,
	 * so both screens show the same framing (host / first joiner left).
	 */
	Fixed,

	/**
	 * Local-player creation order. This machine's own player stays left; the
	 * partner's shadow view lands on the right. The pre-1.6 behaviour, kept as an
	 * option — on a client that mirrors the framing between machines.
	 */
	Own
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

/**
 * One machine's camera at one instant — the whole of what the other machine
 * needs to draw that player's view.
 *
 * The finished camera rather than the inputs to it: the sending machine has
 * already run the game's own camera, spring arm collision included, against the
 * only world where that player's position is authoritative. Sending control
 * rotation instead would mean re-deriving the camera on the receiver against an
 * interpolated proxy pawn, and every hitch in that interpolation would show up
 * as camera jitter rather than as the small positional error it actually is.
 *
 * Quantised because none of the precision that is thrown away is visible: 0.1 uu
 * of position, a fifth of an arcminute of angle, a whole degree of field of view.
 * That leaves roughly twenty bytes, which is what makes a 30 Hz stream cheap.
 */
USTRUCT(BlueprintType)
struct FUSViewSample
{
	GENERATED_BODY()

	/** Camera position, quantised to 0.1 uu. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|View")
	FVector_NetQuantize10 Location = FVector::ZeroVector;

	/** Camera orientation, one unsigned short per axis; roll included, so camera shake survives. */
	UPROPERTY()
	uint16 CompressedPitch = 0;

	UPROPERTY()
	uint16 CompressedYaw = 0;

	UPROPERTY()
	uint16 CompressedRoll = 0;

	/**
	 * Horizontal field of view in hundredths of a degree.
	 *
	 * Whole degrees would be a byte, and it is tempting because half a degree
	 * sounds invisible — it is not. Both views sit side by side on one screen
	 * showing the same kind of camera, and half a degree of field of view is a
	 * visible difference in how far away the character looks when you can compare
	 * the two directly. A hundredth of a degree cannot be seen and costs one byte.
	 */
	UPROPERTY()
	uint16 CompressedFieldOfView = 9000;

	/**
	 * When the sending machine took this sample, in milliseconds since that
	 * machine's own stream started. Never compared against the receiver's clock,
	 * only against other samples from the same sender.
	 *
	 * This is what keeps the network out of the picture. Playing the stream back
	 * against the moments samples *arrived* would make the camera's apparent speed
	 * follow the jitter of the connection: two samples delivered 5 ms apart carry
	 * a whole send interval of real movement, so the view lurches and then crawls.
	 * Spacing them by the moments they were *taken* gives back the sender's own
	 * cadence exactly, and jitter is absorbed by the playback delay instead of
	 * being drawn.
	 *
	 * Four bytes rather than a delta from the previous sample, because most
	 * samples never arrive: a replicated property only ever carries its newest
	 * value, so a receiver that added up deltas would compress its timeline every
	 * time an update was skipped.
	 */
	UPROPERTY()
	uint32 SendTimeMs = 0;

	/**
	 * Bumped by the sender every time the camera jumped instead of moving.
	 *
	 * Nothing in the numbers distinguishes a cut from very fast motion, and
	 * interpolating across a cut sweeps the view through the level between the
	 * two framings. This tells the receiver to throw its history away and land on
	 * the new framing at once.
	 */
	UPROPERTY()
	uint8 CutCounter = 0;

	FRotator GetRotation() const
	{
		return FRotator(
			FRotator::DecompressAxisFromShort(CompressedPitch),
			FRotator::DecompressAxisFromShort(CompressedYaw),
			FRotator::DecompressAxisFromShort(CompressedRoll));
	}

	void SetRotation(const FRotator& Rotation)
	{
		CompressedPitch = FRotator::CompressAxisToShort(Rotation.Pitch);
		CompressedYaw = FRotator::CompressAxisToShort(Rotation.Yaw);
		CompressedRoll = FRotator::CompressAxisToShort(Rotation.Roll);
	}

	float GetFieldOfView() const
	{
		return static_cast<float>(CompressedFieldOfView) / 100.0f;
	}

	void SetFieldOfView(float FieldOfView)
	{
		CompressedFieldOfView = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(FieldOfView * 100.0f), 1, MAX_uint16));
	}
};
