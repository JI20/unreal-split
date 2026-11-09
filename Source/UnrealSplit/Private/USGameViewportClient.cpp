// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "USGameViewportClient.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

#include "UnrealSplitModule.h"
#include "USPlayerViewProxy.h"
#include "USRemoteViewSubsystem.h"
#include "USInputRoutingSubsystem.h"
#include "USShadowLocalPlayer.h"
#include "USSpectatorPlayerController.h"

bool UUSGameViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	const UUSInputRoutingSubsystem* Routing = GetGameInstance() ? GetGameInstance()->GetSubsystem<UUSInputRoutingSubsystem>() : nullptr;
	if (Routing && Routing->IsCapturingDevice()) return true;
	if (Routing && Routing->IsDeviceUnassigned(EventArgs.InputDevice))
	{
		// A front end can explicitly accept unassigned devices through the engine's
		// override hook. They must never fall through to normal gameplay routing.
		FInputKeyEventArgs MutableEventArgs=EventArgs;
		if (OnOverrideInputKey().IsBound()) OnOverrideInputKey().Execute(MutableEventArgs);
		return true;
	}
	return Super::InputKey(EventArgs);
}

bool UUSGameViewportClient::InputAxis(const FInputKeyEventArgs& EventArgs)
{
	const UUSInputRoutingSubsystem* Routing = GetGameInstance() ? GetGameInstance()->GetSubsystem<UUSInputRoutingSubsystem>() : nullptr;
	if (Routing && Routing->IsCapturingDevice()) return true;
	if (Routing && Routing->IsDeviceUnassigned(EventArgs.InputDevice))
	{
		FInputKeyEventArgs MutableEventArgs=EventArgs;
		if (OnOverrideInputAxis().IsBound()) OnOverrideInputAxis().Execute(MutableEventArgs,MutableEventArgs.AmountDepressed,MutableEventArgs.DeltaTime,MutableEventArgs.NumSamples);
		return true;
	}
	return Super::InputAxis(EventArgs);
}

namespace
{
	/**
	 * Slices thinner than this collapse to nothing.
	 *
	 * A two-pixel sliver of a view costs a full scene render — its own shadow and
	 * post passes included — to produce something nobody can read, and the
	 * engine's own guard against a degenerate view rect only catches a rect that
	 * truncates to zero pixels. Snapping instead means a ratio of 0.999 renders
	 * one view rather than two, and the surviving slice takes the whole width so
	 * the views still tile the viewport exactly.
	 */
	constexpr float MinSlotShare = 0.002f;

	float ApplyEasing(EUSViewEasing Easing, float Alpha)
	{
		switch (Easing)
		{
		case EUSViewEasing::EaseIn:
			return FMath::InterpEaseIn(0.0f, 1.0f, Alpha, 2.0f);

		case EUSViewEasing::EaseOut:
			return FMath::InterpEaseOut(0.0f, 1.0f, Alpha, 2.0f);

		case EUSViewEasing::EaseInOut:
			return FMath::InterpEaseInOut(0.0f, 1.0f, Alpha, 2.0f);

		case EUSViewEasing::Linear:
		default:
			return Alpha;
		}
	}
}

void UUSGameViewportClient::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// DeltaTime here is the engine's frame time, not world time, so a blend keeps
	// running while the game is paused or dilated. That is deliberate: the layout
	// is presentation, and a pause menu that froze a half-finished split would
	// leave the screen in a state nothing can get it out of.
	AdvanceBlend(DeltaTime);

	// Both are re-run every frame rather than only when the intent changes: view
	// targets and player controllers appear late on a joining client, so this is
	// also how a merge that arrived before the local controller existed, or whose
	// target actor was not yet relevant, eventually takes effect. Slot order rides
	// along, which is what keeps a view that changes stream — or a shadow view that
	// appears at all — from waiting for the next intent to be placed.
	UpdateMergedViewTargets();
	LayoutPlayers();

	// After the layout, because Auto reads the same topology the layout just walked,
	// and every frame because the transform an Own-mode listener copies is a live
	// camera. UGameViewportClient::Draw reads listener positions later in the frame.
	UpdateAudioListenerPolicy();
}

void UUSGameViewportClient::LayoutPlayers()
{
	// The engine's splitscreen type still drives safe zones, subtitle placement
	// and HUD queries, so it is kept current — but its fixed per-player rectangles
	// are ignored, because offering an arbitrary ratio is the whole point here.
	UpdateActiveSplitscreenType();

	TArray<ULocalPlayer*> Slots;
	GatherSlots(Slots);

	const int32 Player1SlotIndex = FindPlayer1SlotIndex(Slots);

	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		float OriginX = 0.0f;
		float SizeX = 0.0f;
		ComputeSlotRect(SlotIndex, Slots.Num(), Player1SlotIndex, OriginX, SizeX);

		// A zero size is the engine's own way of saying "this player has no view":
		// ULocalPlayer::CalcSceneView bails on it, so the collapsed side of the
		// split and every view behind a merge cost nothing to render at all.
		const bool bHasView = SizeX > 0.0f;
		Slots[SlotIndex]->Origin = FVector2D(bHasView ? OriginX : 0.0f, 0.0f);
		Slots[SlotIndex]->Size = FVector2D(bHasView ? SizeX : 0.0f, bHasView ? 1.0f : 0.0f);
	}
}

void UUSGameViewportClient::SetViewIntent(const FUSViewIntent& NewIntent)
{
	TargetIntent = NewIntent;

	// A replicated intent always wins back control from a local drive.
	bLocalDriveActive = false;

	const float TargetShare = GetTargetSplitRatio();
	const bool bSnap = !bHasAppliedIntent || NewIntent.BlendTime <= 0.0f;

	if (bSnap)
	{
		CurrentShare = TargetShare;
		BlendFromShare = TargetShare;
		BlendDuration = 0.0f;
		BlendElapsed = 0.0f;
	}
	else
	{
		// Re-targeting mid-blend starts from where the screen actually is, so an
		// intent that arrives during another blend cannot snap backwards first.
		BlendFromShare = CurrentShare;
		BlendDuration = NewIntent.BlendTime;
		BlendElapsed = 0.0f;
	}

	bHasAppliedIntent = true;

	UE_LOG(LogUnrealSplit, Log, TEXT("View layout: target Player1Share=%.3f (effective %.3f) bMerged=%d MergedViewTarget=%s over %.2fs %s"),
		NewIntent.Player1Share,
		TargetShare,
		NewIntent.bMerged ? 1 : 0,
		*GetNameSafe(NewIntent.MergedViewTarget.Get()),
		bSnap ? 0.0f : BlendDuration,
		bSnap ? TEXT("(snapped)") : TEXT(""));

	UpdateMergedViewTargets();
	LayoutPlayers();
}

void UUSGameViewportClient::SetSplitRatioLocal(float Player1Share)
{
	bLocalDriveActive = true;
	BlendDuration = 0.0f;
	BlendElapsed = 0.0f;
	CurrentShare = FMath::Clamp(Player1Share, 0.0f, 1.0f);
	BlendFromShare = CurrentShare;

	LayoutPlayers();
}

void UUSGameViewportClient::SetViewOrderMode(EUSViewOrderMode NewMode)
{
	if (ViewOrderMode == NewMode)
	{
		return;
	}

	ViewOrderMode = NewMode;
	UE_LOG(LogUnrealSplit, Log, TEXT("View order on this machine: %s."),
		NewMode == EUSViewOrderMode::Fixed ? TEXT("fixed (player identity, host left)") : TEXT("own (this machine's player left)"));

	LayoutPlayers();
}

EUSAudioListenerMode UUSGameViewportClient::ResolveAutoAudioListenerMode(bool bHasShadowView)
{
	// Auto is defined as "leave every existing session sounding the way it did".
	// A shadow view is the one topology where the second view is not a person in the
	// room, and it is the one that used a single listener before this policy existed.
	return bHasShadowView ? EUSAudioListenerMode::Own : EUSAudioListenerMode::Shared;
}

bool UUSGameViewportClient::HasShadowView() const
{
	if (GEngine == nullptr)
	{
		return false;
	}

	for (const ULocalPlayer* const Player : GEngine->GetGamePlayers(this))
	{
		if (UUSShadowLocalPlayer::IsShadow(Player))
		{
			return true;
		}
	}

	return false;
}

EUSAudioListenerMode UUSGameViewportClient::GetAudioListenerMode() const
{
	if (AudioListenerModeOverride.IsSet())
	{
		return AudioListenerModeOverride.GetValue();
	}

	switch (AudioListenerMode)
	{
	case EUSAudioListenerModeSetting::Own:
		return EUSAudioListenerMode::Own;

	case EUSAudioListenerModeSetting::Shared:
		return EUSAudioListenerMode::Shared;

	case EUSAudioListenerModeSetting::Auto:
	default:
		return ResolveAutoAudioListenerMode(HasShadowView());
	}
}

void UUSGameViewportClient::SetAudioListenerMode(EUSAudioListenerMode NewMode)
{
	const bool bAlreadyThere = AudioListenerModeOverride.IsSet() && AudioListenerModeOverride.GetValue() == NewMode;
	if (bAlreadyThere)
	{
		return;
	}

	AudioListenerModeOverride = NewMode;

	UE_LOG(LogUnrealSplit, Log, TEXT("Audio listeners on this machine: %s."),
		NewMode == EUSAudioListenerMode::Shared
			? TEXT("shared (one set of ears per view, nearest wins per sound)")
			: TEXT("own (one set of ears, on this machine's own view)"));

	UpdateAudioListenerPolicy();
}

void UUSGameViewportClient::ResetAudioListenerModeToConfig()
{
	if (!AudioListenerModeOverride.IsSet())
	{
		return;
	}

	AudioListenerModeOverride.Reset();
	UpdateAudioListenerPolicy();
}

void UUSGameViewportClient::UpdateAudioListenerPolicy()
{
	APlayerController* const PrimaryController = UUSShadowLocalPlayer::GetFirstRealLocalPlayerController(GetGameInstance());

	// Shared is the engine's own arrangement — every view's listener at that view —
	// so it is reached by giving back what was taken rather than by setting anything.
	if (GetAudioListenerMode() == EUSAudioListenerMode::Shared || PrimaryController == nullptr)
	{
		ReleaseHeldAudioListeners();
		return;
	}

	// Read through GetAudioListenerPosition rather than off the camera, so a game
	// that moves its own listener deliberately is followed rather than overruled.
	// The rotation is rebuilt from the axes the engine will rebuild them from again.
	FVector PrimaryLocation;
	FVector PrimaryFront;
	FVector PrimaryRight;
	PrimaryController->GetAudioListenerPosition(PrimaryLocation, PrimaryFront, PrimaryRight);
	const FRotator PrimaryRotation = FRotationMatrix::MakeFromXY(PrimaryFront, PrimaryRight).Rotator();

	TArray<ULocalPlayer*> RealPlayers;
	UUSShadowLocalPlayer::GetRealLocalPlayers(GetGameInstance(), RealPlayers);

	TArray<TWeakObjectPtr<APlayerController>> StillHeld;
	StillHeld.Reserve(HeldAudioListeners.Num());

	for (const ULocalPlayer* const Player : RealPlayers)
	{
		APlayerController* const Controller = Player != nullptr ? Player->PlayerController : nullptr;
		if (Controller == nullptr || Controller == PrimaryController)
		{
			continue;
		}

		// Both listeners in the same place mix identically to one: attenuation picks
		// the nearest, and there is no nearer. Nothing is muted, so a sound that is
		// audible from the primary's ears stays audible.
		Controller->SetAudioListenerOverride(nullptr, PrimaryLocation, PrimaryRotation);
		StillHeld.Add(Controller);
	}

	for (const TWeakObjectPtr<APlayerController>& Held : HeldAudioListeners)
	{
		if (!StillHeld.Contains(Held))
		{
			if (APlayerController* const Controller = Held.Get())
			{
				Controller->ClearAudioListenerOverride();
			}
		}
	}

	HeldAudioListeners = MoveTemp(StillHeld);
}

void UUSGameViewportClient::ReleaseHeldAudioListeners()
{
	for (const TWeakObjectPtr<APlayerController>& Held : HeldAudioListeners)
	{
		if (APlayerController* const Controller = Held.Get())
		{
			Controller->ClearAudioListenerOverride();
		}
	}

	HeldAudioListeners.Reset();
}

FString UUSGameViewportClient::DescribeAudioListenerPolicy() const
{
	const EUSAudioListenerMode Mode = GetAudioListenerMode();
	const bool bShared = Mode == EUSAudioListenerMode::Shared;

	FString Source;
	if (AudioListenerModeOverride.IsSet())
	{
		Source = TEXT("US.AudioMode, holding the configured default off");
	}
	else if (AudioListenerMode == EUSAudioListenerModeSetting::Auto)
	{
		Source = FString::Printf(TEXT("auto, from the %s topology here"),
			HasShadowView() ? TEXT("remote (shadow view)") : TEXT("couch (no shadow view)"));
	}
	else
	{
		Source = TEXT("config default");
	}

	return FString::Printf(TEXT("%s — %s; resolved from %s"),
		bShared ? TEXT("shared") : TEXT("own"),
		bShared
			? TEXT("both views have ears, each sound played once from the nearer")
			: TEXT("one set of ears on this machine's own view"),
		*Source);
}

void UUSGameViewportClient::GetLaidOutSlots(TArray<ULocalPlayer*>& OutSlots) const
{
	GatherSlots(OutSlots);
}

void UUSGameViewportClient::ResetForNewWorld()
{
	// The records are dropped rather than restored: the controllers they refer to
	// belong to the world being left, and their cameras are going away with them.
	MergedViewTargetRecords.Reset();

	// Same for the listeners this policy holds. The chosen mode is a preference for
	// this machine and survives travel; the controllers it was applied to do not, and
	// the next world's are picked up on the first tick there.
	ReleaseHeldAudioListeners();

	TargetIntent = FUSViewIntent();
	CurrentShare = TargetIntent.Player1Share;
	BlendFromShare = CurrentShare;
	BlendDuration = 0.0f;
	BlendElapsed = 0.0f;
	bLocalDriveActive = false;
	bHasAppliedIntent = false;
}

float UUSGameViewportClient::GetTargetSplitRatio() const
{
	// Merging is the same geometry as handing player 1 the whole width, so the
	// collapse animates through the ordinary ratio blend and needs no separate
	// path; only the view targets make a merge different from a 1.0 split.
	return TargetIntent.bMerged ? 1.0f : FMath::Clamp(TargetIntent.Player1Share, 0.0f, 1.0f);
}

float UUSGameViewportClient::GetBlendProgress() const
{
	return BlendDuration > 0.0f ? FMath::Clamp(BlendElapsed / BlendDuration, 0.0f, 1.0f) : 1.0f;
}

void UUSGameViewportClient::AdvanceBlend(float DeltaTime)
{
	if (bLocalDriveActive || BlendDuration <= 0.0f)
	{
		return;
	}

	BlendElapsed = FMath::Min(BlendElapsed + DeltaTime, BlendDuration);

	const float TargetShare = GetTargetSplitRatio();
	CurrentShare = FMath::Lerp(BlendFromShare, TargetShare, ApplyEasing(TargetIntent.Easing, BlendElapsed / BlendDuration));

	if (BlendElapsed >= BlendDuration)
	{
		CurrentShare = TargetShare;
		BlendDuration = 0.0f;
		BlendElapsed = 0.0f;
	}
}

void UUSGameViewportClient::UpdateMergedViewTargets()
{
	for (auto It = MergedViewTargetRecords.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	AActor* const DesiredTarget = TargetIntent.bMerged ? TargetIntent.MergedViewTarget.Get() : nullptr;

	// Merged with nothing to look at: divide the screen as if merged but leave the
	// cameras alone, and try again next frame. This is what a client sees while
	// the target actor is not yet relevant to it — and what everyone sees if the
	// game merged without supplying one, or supplied an actor it never replicated.
	if (TargetIntent.bMerged && DesiredTarget == nullptr)
	{
		return;
	}

	// Every view here, the shadow one included. A shadow view has no camera of its
	// own, but a merge is the case where that stops mattering: merged means every
	// view shows the actor the game named, and the one full-width slot that survives
	// a merge may well be the shadow's, since sides are assigned by player identity
	// rather than by which player is local. Left out, a client would go on watching
	// the raw stream through a merge the host was showing correctly.
	for (ULocalPlayer* Player : GEngine->GetGamePlayers(this))
	{
		APlayerController* const PlayerController = Player != nullptr ? Player->PlayerController : nullptr;
		if (PlayerController == nullptr)
		{
			continue;
		}

		FUSMergedViewTargetRecord* Record = MergedViewTargetRecords.Find(PlayerController);

		// Compared against what this class last asked for rather than against the
		// controller's current view target: while a camera blend is in flight the
		// camera manager still reports the old target, so asking it would re-issue
		// the same request every frame and restart the blend forever.
		const AActor* const AppliedTarget = Record != nullptr ? Record->AppliedTarget.Get() : nullptr;
		if (AppliedTarget == DesiredTarget)
		{
			continue;
		}

		FViewTargetTransitionParams TransitionParams;
		TransitionParams.BlendTime = FMath::Max(TargetIntent.BlendTime, 0.0f);
		TransitionParams.BlendFunction = VTBlend_Cubic;
		TransitionParams.BlendExp = 2.0f;

		if (DesiredTarget != nullptr)
		{
			if (Record == nullptr)
			{
				Record = &MergedViewTargetRecords.Add(PlayerController);
			}

			// Remembered once per merge, not once per frame, so a second merge
			// while still merged cannot overwrite the player's own camera with the
			// first merge's target.
			if (!Record->PreMergeTarget.IsValid())
			{
				Record->PreMergeTarget = PlayerController->GetViewTarget();
			}

			Record->AppliedTarget = DesiredTarget;
			PlayerController->SetViewTarget(DesiredTarget, TransitionParams);
		}
		else if (Record != nullptr)
		{
			// Back to the player's own camera. A pre-merge target that has since
			// been destroyed falls back to the controller, which the camera manager
			// resolves through its pawn — or, for a shadow view, through CalcCamera
			// back to the stream, which is also what its pre-merge target was.
			AActor* const RestoreTarget = Record->PreMergeTarget.Get();
			PlayerController->SetViewTarget(RestoreTarget != nullptr ? RestoreTarget : PlayerController, TransitionParams);
			MergedViewTargetRecords.Remove(PlayerController);
		}
	}
}

void UUSGameViewportClient::ComputeSlotRect(int32 SlotIndex, int32 NumSlots, int32 Player1SlotIndex, float& OutOriginX, float& OutSizeX) const
{
	OutOriginX = 0.0f;
	OutSizeX = 0.0f;

	if (SlotIndex < 0 || SlotIndex >= NumSlots)
	{
		return;
	}

	// One slot is full screen, whatever the ratio says. That is a machine online
	// whose partner's camera stream has not started delivering yet, or one with the
	// remote view switched off: the intent still replicates, is still queryable,
	// and starts dividing the screen the moment a second view exists to divide it
	// between. A menu that force-disabled splitscreen collapses the same way.
	if (NumSlots == 1 || IsSplitscreenForceDisabled())
	{
		if (SlotIndex == 0)
		{
			OutOriginX = 0.0f;
			OutSizeX = 1.0f;
		}

		return;
	}

	float Player1Share = FMath::Clamp(CurrentShare, 0.0f, 1.0f);
	if (Player1Share < MinSlotShare)
	{
		Player1Share = 0.0f;
	}
	else if (Player1Share > 1.0f - MinSlotShare)
	{
		Player1Share = 1.0f;
	}

	// Player1Share is "player 1's width", not "the left slot's width". When player 1
	// sits on the right (Own order on a client), the left edge of the split is at
	// (1 - Player1Share) so that player still receives exactly Player1Share of the
	// screen — and a ratio of 1.0 still collapses the other view to nothing.
	const int32 EffectivePlayer1Slot = Player1SlotIndex != INDEX_NONE ? Player1SlotIndex : 0;
	const float LeftShare = (EffectivePlayer1Slot == 0) ? Player1Share : (1.0f - Player1Share);

	if (SlotIndex == 0)
	{
		OutOriginX = 0.0f;
		OutSizeX = LeftShare;
	}
	else if (SlotIndex == 1)
	{
		OutOriginX = LeftShare;
		OutSizeX = 1.0f - LeftShare;
	}
	// Slots beyond the second get nothing. The game caps a session at two players
	// in total, so a third local view is a bug elsewhere rather than a layout this
	// has to invent; it stays black instead of silently overlapping the others.
}

APlayerState* UUSGameViewportClient::GetViewPlayerState(const ULocalPlayer* Player)
{
	if (Player == nullptr)
	{
		return nullptr;
	}

	// A shadow view has no PlayerState of its own — that is the isolation. The
	// player it shows is the one the stream belongs to, carried on the proxy.
	if (const AUSSpectatorPlayerController* const Spectator = Cast<AUSSpectatorPlayerController>(Player->PlayerController))
	{
		if (const AUSPlayerViewProxy* const Proxy = Spectator->GetViewProxy())
		{
			return Proxy->GetOwningPlayerState();
		}

		return nullptr;
	}

	const APlayerController* const Controller = Player->PlayerController;
	return Controller != nullptr ? Controller->PlayerState : nullptr;
}

int32 UUSGameViewportClient::GetAssignedViewSlot(const ULocalPlayer* Player)
{
	if (Player == nullptr)
	{
		return INDEX_NONE;
	}

	// A shadow view answers first, and online that is the only view that needs to.
	// The slot is a number the server chose for the streamed player and put on the
	// stream, so it is available on the machine that receives it. Two views is the
	// whole range this game can produce, so knowing one side names the other.
	if (const AUSSpectatorPlayerController* const Spectator = Cast<AUSSpectatorPlayerController>(Player->PlayerController))
	{
		if (const AUSPlayerViewProxy* const Proxy = Spectator->GetViewProxy())
		{
			return Proxy->GetViewSlotIndex();
		}
	}

	// Couch has no stream to read, but it does not need one: the authority is this
	// machine, so the slot table is right here. Asking it is what lets a player
	// choose a side and have the screen follow — without this, couch layout would
	// stay on creation order and a chosen side would move only the pawn.
	//
	// Deliberately authority-only. A client's own slot is readable too, from its
	// own replicated stream, but finding that stream means walking the world where
	// the authority answers from a map, and this runs every frame for every view.
	// The shadow stream already settles a client's layout with one lookup.
	const APlayerController* const Controller = Player->PlayerController;

	if (Controller == nullptr || !Controller->HasAuthority())
	{
		return INDEX_NONE;
	}

	const UGameInstance* const GameInstance = Player->GetGameInstance();
	const UUSRemoteViewSubsystem* const RemoteViews = GameInstance != nullptr
		? GameInstance->GetSubsystem<UUSRemoteViewSubsystem>()
		: nullptr;

	return RemoteViews != nullptr ? RemoteViews->GetPlayerViewSlot(Controller) : INDEX_NONE;
}

int32 UUSGameViewportClient::FindPlayer1SlotIndex(const TArray<ULocalPlayer*>& Slots) const
{
	if (Slots.Num() == 0)
	{
		return INDEX_NONE;
	}

	if (Slots.Num() == 2)
	{
		for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
		{
			const int32 AssignedSlot = GetAssignedViewSlot(Slots[SlotIndex]);
			if (!Slots.IsValidIndex(AssignedSlot))
			{
				continue;
			}

			// The other view is whoever this one is not, so a known slot on either
			// side settles both: slot 0's view is this one when the authority gave
			// it slot 0, and the opposite view otherwise.
			return AssignedSlot == 0 ? SlotIndex : 1 - SlotIndex;
		}
	}

	// Nobody has said which slot they own yet — before login, or before the first
	// stream update on a client. Creation order is join order, so the first view is
	// the one to widen.
	return 0;
}

void UUSGameViewportClient::GatherSlots(TArray<ULocalPlayer*>& OutSlots) const
{
	OutSlots.Reset();

	for (ULocalPlayer* Player : GEngine->GetGamePlayers(this))
	{
		if (Player != nullptr && Player->GetViewportClient() == this)
		{
			OutSlots.Add(Player);
		}
	}

	// Own keeps engine creation order: the first real local player was added first
	// and the shadow view second, which reads as "my player on the left". Fewer
	// than two views, or more than this game can produce, have nothing to sort.
	if (ViewOrderMode == EUSViewOrderMode::Own || OutSlots.Num() != 2)
	{
		return;
	}

	// Fixed: the shadow view goes to the slot the authority gave the player it
	// shows, and the local player takes the other one. Both machines put the same
	// player on the same side because both are reading the same replicated number,
	// not each inferring an order from state the engine may renumber.
	for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const int32 AssignedSlot = GetAssignedViewSlot(OutSlots[SlotIndex]);

		if (OutSlots.IsValidIndex(AssignedSlot) && AssignedSlot != SlotIndex)
		{
			Swap(OutSlots[0], OutSlots[1]);
			return;
		}
	}
}
