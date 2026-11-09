// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Info.h"
#include "USViewTypes.h"
#include "USPlayerViewProxy.generated.h"

class APlayerController;
class APlayerState;
class UNetDriver;

/**
 * One player's camera, streamed to every machine in the session.
 *
 * There is one of these per player, spawned by the server and owned by that
 * player's controller. On the machine where that player is local it samples the
 * finished camera and sends it; everywhere else it buffers what arrives and
 * answers with a smoothed view. That is the whole of it — it renders nothing and
 * knows nothing about screen layout.
 *
 * A dedicated actor rather than a component on the pawn or the player state:
 *   - The pawn is the wrong home. It can be null between deaths, it can be
 *     swapped for another pawn, and it can drop out of relevancy while its
 *     player is still very much on screen; the camera stream has to outlive all
 *     three. A camera in a cutscene is not on a pawn at all.
 *   - The player state is the right lifetime but the wrong ownership: adding a
 *     replicated component to whatever player state class the game happens to
 *     use means either forcing a plugin base class on the game or registering
 *     dynamic subobjects for replication, and the plugin's rule is that the game
 *     owes it nothing.
 *   - An actor of its own is owned by the player's controller, which is exactly
 *     what an unreliable client-to-server RPC needs, is always relevant so both
 *     machines see both streams, and can carry its own update frequency without
 *     dragging a pawn's replication along with it.
 *
 * Direction of travel differs per player and is not a session-mode branch, only
 * a question of who knows the answer: the host writes its own camera into a
 * replicated property, and a client sends its own camera to the server, which
 * writes it into the same property for everyone else. Both arrive at the same
 * place through the same code.
 */
UCLASS(Config = Game, NotPlaceable)
class UNREALSPLIT_API AUSPlayerViewProxy : public AInfo
{
	GENERATED_BODY()

public:
	AUSPlayerViewProxy();

	//~ Begin AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	/**
	 * Server-side setup: whose camera this carries, and which view slot that player
	 * owns for the rest of the session.
	 *
	 * The slot is decided once, by the authority, from an identity the server keeps
	 * for the whole game instance — not derived on the receiver. That is what makes
	 * left/right agree on both machines and survive travel and rejoin: a receiver
	 * never has to guess an ordering from state that can be renumbered underneath it.
	 */
	void InitializeForPlayer(APlayerController* OwningController, int32 AssignedViewSlotIndex);

	/**
	 * The view slot this player owns, or INDEX_NONE before it has replicated.
	 *
	 * Plain replicated data rather than an actor reference, so it is readable from
	 * the proxy's very first update — an actor reference such as OwningPlayerState
	 * can take further updates to resolve, and ordering that waited for it would
	 * paint one framing and then swap.
	 */
	int32 GetViewSlotIndex() const;

	/**
	 * Moves this player to another view slot, authority only.
	 *
	 * Assigned once at login and then left alone for most of a session, but not
	 * immutable: a game that lets players pick a side needs the authority to be
	 * able to change its mind, and the replicated property is what carries that
	 * decision to the other machine. Layout follows on its own, because the
	 * viewport client re-runs its slot maths every frame.
	 */
	void SetViewSlotIndex(int32 NewViewSlotIndex);

	/**
	 * True on the one machine where this player is a local player, which is the
	 * machine that samples and sends. Everywhere else this proxy is a receiver.
	 *
	 * Asked of the owner rather than remembered, because the answer is a property
	 * of the machine: the owning controller is local on exactly one of them, and
	 * on a machine the controller never reached it reads as null, which is the
	 * same answer.
	 */
	bool IsSendingMachine() const;

	/** The player this stream belongs to, for diagnostics; null until it replicates. */
	APlayerState* GetOwningPlayerState() const;

	/**
	 * The view to draw for this player right now, smoothed and delayed.
	 * False until the first sample has arrived, when there is nothing to draw.
	 */
	bool GetInterpolatedView(FMinimalViewInfo& OutView);

	/** True once, after a cut, so the renderer can be told not to blend across it. */
	bool ConsumePendingCameraCut();

	/** Seconds since anything last arrived on this stream; negative before the first. */
	float GetNewestSampleAgeSeconds() const;

	/** Samples currently buffered for interpolation. */
	int32 GetBufferedSampleCount() const;

	/**
	 * The rate samples are arriving at, in Hz, smoothed over recent intervals.
	 *
	 * Measured from the gaps between the senders' own timestamps rather than across
	 * the buffer, because the buffer is trimmed to what playback can still reach and
	 * is only a handful of samples deep in the steady state — far too short a window
	 * to read a rate off. A figure well below SendRateHz means samples are being
	 * dropped or coalesced, or the sending machine is not ticking often enough to
	 * produce them, which is the thing worth knowing.
	 */
	float GetMeasuredSampleRateHz() const;

	/** Seconds of deliberate delay this proxy plays back behind the newest sample. */
	float GetInterpolationDelaySeconds() const { return InterpolationDelaySeconds; }

	/** True once the playback clock is running, so an offset reading means something. */
	bool HasLivePlayback() const;

	/**
	 * How far behind the newest buffered sample playback currently sits, in seconds.
	 *
	 * This is the live interpolation offset — the quantity that should hover near
	 * InterpolationDelaySeconds and is what distinguishes "the delay grew" from
	 * "samples stopped arriving" (GetNewestSampleAgeSeconds) and from "the process
	 * is starved" (measured Hz collapsed). Briefly negative while the buffer is dry
	 * and the playhead has run past the newest sample. Only meaningful while
	 * HasLivePlayback.
	 */
	float GetPlaybackOffsetSeconds() const;

private:
	/**
	 * Client to server, unreliable on purpose: a camera sample is worthless the
	 * moment a newer one exists, so a reliable channel would only queue stale
	 * frames and stall the connection when the queue overflowed.
	 */
	UFUNCTION(Server, Unreliable, WithValidation)
	void ServerUpdateViewSample(FUSViewSample Sample);

	UFUNCTION()
	void OnRep_ViewSample();

	/**
	 * Tells the subsystem a slot arrived, so a side picker on this machine redraws.
	 *
	 * Needed because a receiver has no other signal: the authority's decision is
	 * only ever visible here as this property changing, and a UI that waited for a
	 * game-side event would wait forever on a client.
	 */
	UFUNCTION()
	void OnRep_ViewSlotIndex();

	/** Takes one sample from either arrival path into the interpolation buffer. */
	void AcceptSample(const FUSViewSample& Sample);

	/** Reads this machine's finished camera and sends it, at most SendRateHz times a second. */
	void SampleAndSend();

	/** The local player controller whose camera this proxy sends, or null on a receiver. */
	APlayerController* GetSendingController() const;

	/**
	 * The stream itself. One property so a sample's position and orientation are
	 * compared and sent together; split across properties, a frame that moved but
	 * had not yet turned would read on the other machine as a lurch.
	 *
	 * Not sent back to the machine that produced it: the owner already has a
	 * better copy of its own camera, and echoing it would double the cost of the
	 * stream for nothing.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ViewSample)
	FUSViewSample ViewSample;

	/** Which player this is, so a machine can tell one stream from another. */
	UPROPERTY(Replicated)
	TObjectPtr<APlayerState> OwningPlayerState;

	/**
	 * Which view slot this player owns, authority-assigned. MAX_uint8 until then,
	 * which reads as INDEX_NONE through the accessor.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ViewSlotIndex)
	uint8 ViewSlotIndex = MAX_uint8;

	/** Moves the playback clock on by one frame and reports where it now points. */
	double AdvancePlaybackClock();

	/** Drops samples the playhead has fully passed; keeps one on each side of it. */
	void TrimSamplesBehindPlayhead(double PlayheadSenderTime);

	/** Clears buffered history and invalidates the playback clock (cut / stall / travel). */
	void ResetPlaybackState();

	/** One buffered sample, keyed by the moment the sender took it. */
	struct FTimedViewSample
	{
		FUSViewSample Sample;

		/** Seconds on the sender's clock. The timeline playback runs along. */
		double SenderTime = 0.0;
	};

	/**
	 * Recent samples, oldest first, spaced as the sender took them. Playback runs
	 * InterpolationDelaySeconds behind the newest one, so ordinary network jitter
	 * lands inside the buffer and never reaches the screen. The delay costs
	 * nothing visible: nobody can tell that a view they do not control is a tenth
	 * of a second old.
	 */
	TArray<FTimedViewSample> SampleBuffer;

	/**
	 * Where playback currently is, on the sender's timeline.
	 *
	 * Advanced by this machine's own frame time and nudged, never jumped, towards
	 * one delay behind the newest sample. Nudging the *rate* is what keeps this
	 * smooth: a clock that were set directly to the newest arrival each frame
	 * would inherit every millisecond of jitter, while a clock that only ever
	 * drifts a few percent fast or slow absorbs the same jitter invisibly. A gap
	 * too large to walk off — wall-clock stall, sparse ticks after focus loss, a
	 * wrapped sender clock, or a coalesced sample jump — is resynced outright so
	 * the offset cannot ratchet upward across minutes of a starved process.
	 */
	double PlaybackSenderTime = 0.0;
	bool bPlaybackClockValid = false;

	/** Purely for diagnostics: when anything last arrived, on this machine's clock. */
	double LastArrivalRealTime = 0.0;

	/**
	 * Smoothed gap between consecutive samples, on the sender's timeline, which is
	 * what the reported receive rate is derived from. Re-seeded rather than blended
	 * across a discontinuity, so one stall does not colour the reading afterwards.
	 */
	double SmoothedSampleIntervalSeconds = 0.0;
	bool bHasSampleInterval = false;

	/** Frame the clock last advanced on, so several camera queries in one frame agree. */
	uint64 LastAdvanceFrame = 0;
	double LastAdvanceRealTime = 0.0;

	/** Sender side: the moment its stream started, which its timestamps count from. */
	double SendEpochRealTime = 0.0;
	double LastSendRealTime = 0.0;

	uint8 LastSentCutCounter = 0;
	uint8 LastAcceptedCutCounter = 0;
	bool bHasAcceptedSample = false;
	bool bPendingCameraCut = false;

	/** Last sample this machine sent, to measure the jump that marks a cut. */
	FVector LastSentLocation = FVector::ZeroVector;
	bool bHasSentSample = false;

	/** How often a camera is sampled and sent, per second. */
	UPROPERTY(Config)
	float SendRateHz = 30.0f;

	/**
	 * How far behind the sender the receiver plays the stream back.
	 *
	 * Has to exceed the jitter of the arrival cadence or the buffer runs dry
	 * between samples, which is what rubber-banding looks like. Three send
	 * intervals covers the profiles worth caring about, the 100 ms one included.
	 */
	UPROPERTY(Config)
	float InterpolationDelaySeconds = 0.1f;

	/**
	 * How hard the playback clock is pulled back towards its target delay, per
	 * second of error. Small enough to be invisible, large enough to walk off a
	 * frame of drift in well under a second.
	 */
	UPROPERTY(Config)
	float ClockCorrectionGain = 2.0f;

	/** Most the playback clock will run fast or slow, as a fraction of real time. */
	UPROPERTY(Config)
	float MaxClockCorrection = 0.15f;

	/** Error past which the clock is set rather than nudged, in seconds. */
	UPROPERTY(Config)
	float ClockResyncThresholdSeconds = 0.5f;

	/**
	 * Longest wall-clock step the playback clock will integrate in one advance.
	 *
	 * A background process that wakes only a few times a second would otherwise
	 * clamp each step and fall behind the sender timeline until the error
	 * threshold fired — a sawtooth of growing lag. Any wake longer than this is
	 * treated as a stall and resynced instead, which is also the better answer for
	 * a genuinely slow receiver: it shows the newest framing one delay back rather
	 * than falling further behind every frame.
	 *
	 * Not to be lowered much. Set below a frame time it would resync every frame,
	 * which is arrival-keyed playback and brings back exactly the jitter the
	 * sender-timeline design exists to avoid. At the default no playable frame rate
	 * comes near it.
	 */
	UPROPERTY(Config)
	float MaxPlaybackAdvanceSeconds = 0.25f;

	/**
	 * Floor for the gap, in sender time, past which a newly arrived sample counts as
	 * a stall rather than motion: intermediate samples were coalesced or the receiver
	 * was asleep, and lerping across such a gap crawls the camera through the missing
	 * motion in slow motion. Snapping lands on the new framing at the configured delay.
	 */
	UPROPERTY(Config)
	float SampleGapResyncSeconds = 0.35f;

	/**
	 * The same threshold expressed against the cadence the stream is actually
	 * running at, whichever is larger.
	 *
	 * Without this a slow but perfectly regular sender is mistaken for a stalling
	 * fast one: a machine pinned at three frames a second produces samples a third
	 * of a second apart quite legitimately, and calling each one a cut would replace
	 * smooth low-rate motion with three jumps a second — worse than the crawl this
	 * is here to prevent, and in precisely the case worth looking at. A real stall is
	 * several of the stream's own intervals long whatever that interval happens to be.
	 */
	UPROPERTY(Config)
	float SampleGapResyncIntervalMultiplier = 4.0f;

	/** A sampled camera that moved further than this in one interval was cut, not moved. */
	UPROPERTY(Config)
	float CutDistanceThreshold = 500.0f;

	/** Buffer bound; at 30 Hz this is a second of history, far more than the delay needs. */
	UPROPERTY(Config)
	int32 MaxBufferedSamples = 32;
};
