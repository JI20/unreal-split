// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USPlayerViewProxy.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"

#include "UnrealSplitModule.h"
#include "USRemoteViewSubsystem.h"

namespace
{
	void ApplySampleToView(const FUSViewSample& Sample, FMinimalViewInfo& OutView)
	{
		OutView.Location = Sample.Location;
		OutView.Rotation = Sample.GetRotation();
		OutView.FOV = Sample.GetFieldOfView();
	}
}

AUSPlayerViewProxy::AUSPlayerViewProxy()
{
	bReplicates = true;

	// Always relevant because relevancy here is the wrong question: this actor
	// sits nowhere in particular and both machines need both streams regardless
	// of where the players are standing.
	bAlwaysRelevant = true;
	SetReplicateMovement(false);

	PrimaryActorTick.bCanEverTick = true;

	// As late in the frame as an actor tick goes. The engine finishes the camera
	// after every tick group has run (UWorld::Tick updates the camera managers
	// last), so any sample necessarily reads the camera as it was left at the end
	// of the previous frame; this keeps that gap to one frame instead of letting it
	// grow with tick order. A frame of staleness is irrelevant next to the playback
	// delay the receiver adds anyway.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void AUSPlayerViewProxy::BeginPlay()
{
	Super::BeginPlay();

	// The stream's own cadence, independent of anything else in the level: the
	// actor is considered for replication exactly as often as it has something
	// new to say.
	SetNetUpdateFrequency(FMath::Max(SendRateHz, 1.0f));

	SendEpochRealTime = FPlatformTime::Seconds();

	UE_LOG(LogUnrealSplit, Log, TEXT("View proxy '%s' began play as a %s (player '%s', %.0f Hz, %.0f ms playback delay)."),
		*GetName(),
		IsSendingMachine() ? TEXT("sender") : TEXT("receiver"),
		*GetNameSafe(OwningPlayerState.Get()),
		SendRateHz,
		InterpolationDelaySeconds * 1000.0f);
}

void AUSPlayerViewProxy::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Told rather than discovered: a partner who left, a level that travelled and
	// a connection that dropped all arrive here, and the shadow view has to go
	// away in every one of those cases.
	const UWorld* const World = GetWorld();
	const UGameInstance* const GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;

	if (UUSRemoteViewSubsystem* const RemoteView = GameInstance != nullptr ? GameInstance->GetSubsystem<UUSRemoteViewSubsystem>() : nullptr)
	{
		RemoteView->NotifyRemoteStreamLost(this);
	}

	Super::EndPlay(EndPlayReason);
}

void AUSPlayerViewProxy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Nothing to stream to, and nothing that could receive one: a standalone
	// machine has no connection at all. This is what keeps a couch machine's
	// per-frame work exactly what it was before the remote view existed — the
	// proxies still exist, ready for the moment it hosts or joins, but they do no
	// work until there is somewhere for a sample to go.
	if (GetNetMode() == NM_Standalone)
	{
		return;
	}

	if (IsSendingMachine())
	{
		SampleAndSend();
	}
}

void AUSPlayerViewProxy::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Skipping the owner is not an optimisation detail worth hiding: the owning
	// machine is the one that produced the sample, it already has a fresher copy,
	// and its own proxy therefore never delivers anything to it. The shadow view
	// relies on that — a stream that delivers to this machine is by definition
	// somebody else's.
	DOREPLIFETIME_CONDITION(AUSPlayerViewProxy, ViewSample, COND_SkipOwner);
	DOREPLIFETIME(AUSPlayerViewProxy, OwningPlayerState);
	DOREPLIFETIME(AUSPlayerViewProxy, ViewSlotIndex);
}

int32 AUSPlayerViewProxy::GetViewSlotIndex() const
{
	return ViewSlotIndex != MAX_uint8 ? static_cast<int32>(ViewSlotIndex) : INDEX_NONE;
}

void AUSPlayerViewProxy::InitializeForPlayer(APlayerController* OwningController, int32 AssignedViewSlotIndex)
{
	if (!HasAuthority())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("View proxy '%s' was initialised off the authority; ignored."), *GetName());
		return;
	}

	if (OwningController == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("View proxy '%s' was initialised without a controller; it will never carry a camera."), *GetName());
		return;
	}

	// Ownership is what lets the owning client send its own camera up: an
	// unreliable server RPC is only legal from the machine that owns the actor.
	SetOwner(OwningController);
	OwningPlayerState = OwningController->PlayerState;

	ViewSlotIndex = (AssignedViewSlotIndex >= 0 && AssignedViewSlotIndex < MAX_uint8)
		? static_cast<uint8>(AssignedViewSlotIndex)
		: MAX_uint8;
}

bool AUSPlayerViewProxy::IsSendingMachine() const
{
	return GetSendingController() != nullptr;
}

APlayerState* AUSPlayerViewProxy::GetOwningPlayerState() const
{
	return OwningPlayerState.Get();
}

APlayerController* AUSPlayerViewProxy::GetSendingController() const
{
	APlayerController* const Controller = Cast<APlayerController>(GetOwner());

	// Local on exactly one machine. On the server the host's own controller
	// answers true and a client's controller answers false; on a client the
	// remote player's controller was never replicated at all, so the owner reads
	// as null and the answer is the same.
	return (Controller != nullptr && Controller->IsLocalController()) ? Controller : nullptr;
}

void AUSPlayerViewProxy::SampleAndSend()
{
	const APlayerController* const Controller = GetSendingController();
	APlayerCameraManager* const CameraManager = Controller != nullptr ? Controller->PlayerCameraManager : nullptr;

	if (CameraManager == nullptr)
	{
		return;
	}

	// Real time on both ends, deliberately. Sampling and playback then share one
	// clock that no other system can dilate or pause underneath them, and the
	// receiver needs no agreement with the sender about what time it is — only
	// about the order things arrived in.
	const double Now = FPlatformTime::Seconds();
	const double SendInterval = SendRateHz > 0.0f ? 1.0 / static_cast<double>(SendRateHz) : 0.0;

	if (bHasSentSample && (Now - LastSendRealTime) < SendInterval)
	{
		return;
	}

	LastSendRealTime = Now;

	FUSViewSample Sample;
	Sample.Location = CameraManager->GetCameraLocation();
	Sample.SetRotation(CameraManager->GetCameraRotation());
	Sample.SetFieldOfView(CameraManager->GetFOVAngle());

	// Taken now rather than when this is written into the property or arrives, so
	// the receiver can reconstruct the cadence this machine actually sampled at.
	Sample.SendTimeMs = static_cast<uint32>(FMath::RoundToDouble((Now - SendEpochRealTime) * 1000.0));

	// Two ways to learn about a cut, because neither is enough alone: the camera
	// manager knows about the ones the game announced, and a jump larger than any
	// plausible movement in one interval catches the rest, such as a view target
	// swapped with no blend.
	const bool bJumped = bHasSentSample && FVector::Dist(LastSentLocation, Sample.Location) > CutDistanceThreshold;

	if (CameraManager->bGameCameraCutThisFrame || bJumped)
	{
		++LastSentCutCounter;
	}

	Sample.CutCounter = LastSentCutCounter;
	LastSentLocation = Sample.Location;
	bHasSentSample = true;

	if (HasAuthority())
	{
		ViewSample = Sample;
	}
	else
	{
		ServerUpdateViewSample(Sample);
	}
}

bool AUSPlayerViewProxy::ServerUpdateViewSample_Validate(FUSViewSample Sample)
{
	// A hostile sample cannot reach gameplay from here — the worst it can do is
	// point one spectator view somewhere absurd. A non-finite one would reach the
	// renderer through the interpolator, though, so it is refused outright rather
	// than clamped into something plausible-looking.
	return !Sample.Location.ContainsNaN() && Sample.CompressedFieldOfView > 0;
}

void AUSPlayerViewProxy::ServerUpdateViewSample_Implementation(FUSViewSample Sample)
{
	if (!HasAuthority())
	{
		return;
	}

	// Written so it reaches the other machines, and applied by hand because a
	// replicated property never notifies the machine that wrote it. On a listen
	// server this line is the whole reason the host can see the client at all —
	// the same rule AUSViewState follows for view intent.
	ViewSample = Sample;
	AcceptSample(Sample);
}

void AUSPlayerViewProxy::OnRep_ViewSample()
{
	AcceptSample(ViewSample);
}

void AUSPlayerViewProxy::ResetPlaybackState()
{
	SampleBuffer.Reset();
	bPlaybackClockValid = false;
	PlaybackSenderTime = 0.0;
	LastAdvanceFrame = 0;
	LastAdvanceRealTime = 0.0;

	// Re-seeded, not carried over: the interval across a stall says nothing about
	// the rate the stream runs at either side of it.
	bHasSampleInterval = false;
}

void AUSPlayerViewProxy::TrimSamplesBehindPlayhead(double PlayheadSenderTime)
{
	// The playhead can only ever move forward between resyncs, so a sample the one
	// behind it has also passed can never be read again. Keeping the last sample at
	// or before the playhead is what the "before" side of the lerp needs; two
	// samples are always left so the interpolator still has a segment to work with.
	while (SampleBuffer.Num() >= 3 && SampleBuffer[1].SenderTime <= PlayheadSenderTime)
	{
		SampleBuffer.RemoveAt(0, 1, EAllowShrinking::No);
	}
}

void AUSPlayerViewProxy::AcceptSample(const FUSViewSample& Sample)
{
	const double SenderTime = static_cast<double>(Sample.SendTimeMs) / 1000.0;
	LastArrivalRealTime = FPlatformTime::Seconds();

	bool bDiscontinuity = bHasAcceptedSample && Sample.CutCounter != LastAcceptedCutCounter;

	if (!SampleBuffer.IsEmpty())
	{
		const double LastSenderTime = SampleBuffer.Last().SenderTime;

		if (SenderTime <= LastSenderTime)
		{
			const double Backwards = LastSenderTime - SenderTime;

			// A sample older than one already held is either an unreliable RPC that
			// overtook nothing and arrived late, or a sender whose millisecond counter
			// wrapped after seven weeks of uptime. A small step back is the first, and
			// dropping it keeps the timeline sorted, which everything below assumes; a
			// large one is the second, and the only sane response is to start again.
			if (Backwards < 1.0)
			{
				return;
			}

			bDiscontinuity = true;
		}
		else
		{
			// Property replication delivers only the newest value. A receiver that
			// slept (background window, hitch) wakes to a sample far ahead of the
			// last one it saw; lerping that gap plays the missing motion as a long
			// slow crawl. Treat it like a cut so playback lands on the new framing
			// at the configured delay instead.
			//
			// Measured against the cadence this stream runs at as well as against
			// the configured floor, because a sender that is merely slow is not
			// stalling: three samples a second, a third of a second apart, are that
			// stream working normally, and snapping on each would trade smooth
			// low-rate motion for three jumps a second. Before any cadence has been
			// observed the floor is all there is to go on.
			const double GapThreshold = bHasSampleInterval
				? FMath::Max(
					static_cast<double>(SampleGapResyncSeconds),
					SmoothedSampleIntervalSeconds * FMath::Max(static_cast<double>(SampleGapResyncIntervalMultiplier), 2.0))
				: static_cast<double>(SampleGapResyncSeconds);

			if ((SenderTime - LastSenderTime) > GapThreshold)
			{
				bDiscontinuity = true;
			}
		}
	}

	if (bDiscontinuity)
	{
		ResetPlaybackState();
		bPendingCameraCut = true;
	}

	LastAcceptedCutCounter = Sample.CutCounter;

	const bool bFirstSample = !bHasAcceptedSample;
	bHasAcceptedSample = true;

	// Measured before the sample goes in, while the previous one is still the last
	// entry, and only for a step the timeline actually walked — a discontinuity
	// emptied the buffer above and leaves nothing to measure against.
	if (!SampleBuffer.IsEmpty())
	{
		const double Interval = FMath::Clamp(SenderTime - SampleBuffer.Last().SenderTime, 0.001, 2.0);

		SmoothedSampleIntervalSeconds = bHasSampleInterval
			? FMath::Lerp(SmoothedSampleIntervalSeconds, Interval, 0.2)
			: Interval;
		bHasSampleInterval = true;
	}

	SampleBuffer.Add(FTimedViewSample{Sample, SenderTime});

	const int32 MaxSamples = FMath::Max(MaxBufferedSamples, 2);
	if (SampleBuffer.Num() > MaxSamples)
	{
		SampleBuffer.RemoveAt(0, SampleBuffer.Num() - MaxSamples, EAllowShrinking::No);
	}

	if (bFirstSample)
	{
		// The first frame of a view is a cut from nothing.
		bPendingCameraCut = true;

		const UWorld* const World = GetWorld();
		const UGameInstance* const GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;

		if (UUSRemoteViewSubsystem* const RemoteView = GameInstance != nullptr ? GameInstance->GetSubsystem<UUSRemoteViewSubsystem>() : nullptr)
		{
			// Announced on the first sample rather than on BeginPlay, so the shadow
			// view is never created before there is something for it to show.
			RemoteView->NotifyRemoteStreamLive(this);
		}
	}
}

double AUSPlayerViewProxy::AdvancePlaybackClock()
{
	const double Now = FPlatformTime::Seconds();

	// One delay behind the newest thing the sender said. Chasing the newest sample
	// itself would leave nothing to interpolate towards and every hiccup would show.
	const double Target = SampleBuffer.Last().SenderTime - static_cast<double>(InterpolationDelaySeconds);

	// Idempotent within a frame. The camera is normally asked for once per frame,
	// but anything that asks a second time must get the same answer rather than
	// pushing the clock further along.
	if (LastAdvanceFrame == GFrameCounter && bPlaybackClockValid)
	{
		return PlaybackSenderTime;
	}

	const double WallDelta = bPlaybackClockValid ? FMath::Max(0.0, Now - LastAdvanceRealTime) : 0.0;
	const double MaxAdvance = FMath::Max(static_cast<double>(MaxPlaybackAdvanceSeconds), 0.01);
	const double ResyncThreshold = static_cast<double>(ClockResyncThresholdSeconds);

	LastAdvanceFrame = GFrameCounter;
	LastAdvanceRealTime = Now;

	// Resync rather than integrate a clamped step when:
	//   - the clock is fresh (cut / gap / first sample),
	//   - wall time since the last advance exceeded what we are willing to integrate
	//     in one go (background wakes, hitches) — clamping here used to let the
	//     offset ratchet toward the error threshold every sparse frame,
	//   - or the playhead is already further from its target than a nudge can fix.
	if (!bPlaybackClockValid
		|| WallDelta > MaxAdvance
		|| FMath::Abs(Target - PlaybackSenderTime) > ResyncThreshold)
	{
		// A resync moves the playhead rather than walking it, so the next frame can
		// be drawn from a framing some distance from the one just drawn. Past a
		// playback delay's worth of timeline that is a jump, not motion, and the
		// renderer has to be told: temporal anti-aliasing and motion blur would
		// otherwise smear the two together. Only when the clock was already running —
		// a fresh clock has had a cut flagged by whatever invalidated it.
		if (bPlaybackClockValid
			&& FMath::Abs(Target - PlaybackSenderTime) > static_cast<double>(InterpolationDelaySeconds))
		{
			bPendingCameraCut = true;
		}

		PlaybackSenderTime = Target;
		bPlaybackClockValid = true;
		return PlaybackSenderTime;
	}

	const double Error = Target - PlaybackSenderTime;
	const double Correction = FMath::Clamp(
		Error * static_cast<double>(ClockCorrectionGain),
		-static_cast<double>(MaxClockCorrection),
		static_cast<double>(MaxClockCorrection));

	PlaybackSenderTime += WallDelta * (1.0 + Correction);

	return PlaybackSenderTime;
}

bool AUSPlayerViewProxy::GetInterpolatedView(FMinimalViewInfo& OutView)
{
	if (SampleBuffer.Num() == 0)
	{
		return false;
	}

	const double RenderTime = AdvancePlaybackClock();
	TrimSamplesBehindPlayhead(RenderTime);

	const FTimedViewSample& Newest = SampleBuffer.Last();

	// Past the newest sample: either the delay has not filled yet or the stream
	// stopped. Holding still reads as a pause and recovers cleanly; extrapolating
	// would read as the camera wandering off on its own and then snapping back.
	if (RenderTime >= Newest.SenderTime)
	{
		ApplySampleToView(Newest.Sample, OutView);
		return true;
	}

	const FTimedViewSample& Oldest = SampleBuffer[0];
	if (RenderTime <= Oldest.SenderTime)
	{
		ApplySampleToView(Oldest.Sample, OutView);
		return true;
	}

	for (int32 Index = SampleBuffer.Num() - 1; Index > 0; --Index)
	{
		const FTimedViewSample& After = SampleBuffer[Index];
		const FTimedViewSample& Before = SampleBuffer[Index - 1];

		if (RenderTime < Before.SenderTime)
		{
			continue;
		}

		const double Span = After.SenderTime - Before.SenderTime;
		const float Alpha = Span > UE_DOUBLE_SMALL_NUMBER
			? static_cast<float>((RenderTime - Before.SenderTime) / Span)
			: 1.0f;

		OutView.Location = FMath::Lerp(FVector(Before.Sample.Location), FVector(After.Sample.Location), Alpha);

		// Slerped as quaternions rather than lerped as rotators: rotator lerp
		// takes the long way round whenever an axis crosses 180 degrees, which is
		// a full spin of the spectated view for no reason.
		OutView.Rotation = FQuat::Slerp(
			Before.Sample.GetRotation().Quaternion(),
			After.Sample.GetRotation().Quaternion(),
			Alpha).Rotator();

		OutView.FOV = FMath::Lerp(Before.Sample.GetFieldOfView(), After.Sample.GetFieldOfView(), Alpha);

		return true;
	}

	ApplySampleToView(Newest.Sample, OutView);
	return true;
}

bool AUSPlayerViewProxy::ConsumePendingCameraCut()
{
	const bool bCut = bPendingCameraCut;
	bPendingCameraCut = false;
	return bCut;
}

float AUSPlayerViewProxy::GetNewestSampleAgeSeconds() const
{
	if (!bHasAcceptedSample)
	{
		return -1.0f;
	}

	return static_cast<float>(FPlatformTime::Seconds() - LastArrivalRealTime);
}

bool AUSPlayerViewProxy::HasLivePlayback() const
{
	return bPlaybackClockValid && !SampleBuffer.IsEmpty();
}

float AUSPlayerViewProxy::GetPlaybackOffsetSeconds() const
{
	if (!HasLivePlayback())
	{
		return 0.0f;
	}

	return static_cast<float>(SampleBuffer.Last().SenderTime - PlaybackSenderTime);
}

int32 AUSPlayerViewProxy::GetBufferedSampleCount() const
{
	return SampleBuffer.Num();
}

float AUSPlayerViewProxy::GetMeasuredSampleRateHz() const
{
	if (!bHasSampleInterval || SmoothedSampleIntervalSeconds <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0f;
	}

	return static_cast<float>(1.0 / SmoothedSampleIntervalSeconds);
}
