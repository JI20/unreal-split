// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USGameViewportClient.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"

#include "UnrealSplitModule.h"

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
	// target actor was not yet relevant, eventually takes effect.
	UpdateMergedViewTargets();
	LayoutPlayers();
}

void UUSGameViewportClient::LayoutPlayers()
{
	// The engine's splitscreen type still drives safe zones, subtitle placement
	// and HUD queries, so it is kept current — but its fixed per-player rectangles
	// are ignored, because offering an arbitrary ratio is the whole point here.
	UpdateActiveSplitscreenType();

	TArray<ULocalPlayer*> Slots;
	GatherSlots(Slots);

	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		float OriginX = 0.0f;
		float SizeX = 0.0f;
		ComputeSlotRect(SlotIndex, Slots.Num(), OriginX, SizeX);

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

void UUSGameViewportClient::ResetForNewWorld()
{
	// The records are dropped rather than restored: the controllers they refer to
	// belong to the world being left, and their cameras are going away with them.
	MergedViewTargetRecords.Reset();

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
			// been destroyed falls back to the controller, which the camera
			// manager resolves to its pawn.
			AActor* const RestoreTarget = Record->PreMergeTarget.Get();
			PlayerController->SetViewTarget(RestoreTarget != nullptr ? RestoreTarget : PlayerController, TransitionParams);
			MergedViewTargetRecords.Remove(PlayerController);
		}
	}
}

void UUSGameViewportClient::ComputeSlotRect(int32 SlotIndex, int32 NumSlots, float& OutOriginX, float& OutSizeX) const
{
	OutOriginX = 0.0f;
	OutSizeX = 0.0f;

	if (SlotIndex < 0 || SlotIndex >= NumSlots)
	{
		return;
	}

	// One slot is full screen, whatever the ratio says. This is the online case
	// until the 1.6 shadow view gives a remote player's image a slot of its own:
	// the intent still replicates, is still queryable, and starts dividing the
	// screen the moment a second view exists to divide it between.
	// A menu that force-disabled splitscreen collapses the same way.
	if (NumSlots == 1 || IsSplitscreenForceDisabled())
	{
		if (SlotIndex == 0)
		{
			OutOriginX = 0.0f;
			OutSizeX = 1.0f;
		}

		return;
	}

	float Share = FMath::Clamp(CurrentShare, 0.0f, 1.0f);
	if (Share < MinSlotShare)
	{
		Share = 0.0f;
	}
	else if (Share > 1.0f - MinSlotShare)
	{
		Share = 1.0f;
	}

	if (SlotIndex == 0)
	{
		OutOriginX = 0.0f;
		OutSizeX = Share;
	}
	else if (SlotIndex == 1)
	{
		OutOriginX = Share;
		OutSizeX = 1.0f - Share;
	}
	// Slots beyond the second get nothing. The game caps a session at two players
	// in total, so a third local view is a bug elsewhere rather than a layout this
	// has to invent; it stays black instead of silently overlapping the others.
}

void UUSGameViewportClient::GatherSlots(TArray<ULocalPlayer*>& OutSlots) const
{
	OutSlots.Reset();

	// Engine order is slot order, and it is the same order US.AddLocalPlayer and
	// the input routing subsystem talk about, so "player 1" means one thing
	// everywhere: slot 0.
	for (ULocalPlayer* Player : GEngine->GetGamePlayers(this))
	{
		if (Player != nullptr && Player->GetViewportClient() == this)
		{
			OutSlots.Add(Player);
		}
	}
}
