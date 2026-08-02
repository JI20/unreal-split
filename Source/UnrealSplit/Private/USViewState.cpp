// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USViewState.h"

#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "UObject/Class.h"

#include "USViewSubsystem.h"
#include "UnrealSplitModule.h"

AUSViewState::AUSViewState()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	PrimaryActorTick.bCanEverTick = false;
}

void AUSViewState::BeginPlay()
{
	Super::BeginPlay();

	// A RepNotify is skipped on initial replication when the replicated value
	// still equals the class default, which is exactly the common case here: a
	// session that has not changed its layout yet. Applying once on arrival gives
	// a joining client the current intent either way, and the viewport client
	// snaps rather than blends the first intent it sees, so a client that joins
	// mid-session starts at the layout everyone else is already at.
	if (!HasAuthority())
	{
		OnRep_ViewIntent();
	}
}

void AUSViewState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUSViewState, ViewIntent);
}

void AUSViewState::SetSplitRatio(float Player1Share, float BlendTime, EUSViewEasing Easing)
{
	FUSViewIntent NewIntent = ViewIntent;
	NewIntent.Player1Share = FMath::Clamp(Player1Share, 0.0f, 1.0f);
	NewIntent.BlendTime = FMath::Max(BlendTime, 0.0f);
	NewIntent.Easing = Easing;

	SetViewIntentOnAuthority(NewIntent);
}

void AUSViewState::SetMerged(AActor* ViewTarget, float BlendTime, EUSViewEasing Easing)
{
	FUSViewIntent NewIntent = ViewIntent;
	NewIntent.bMerged = true;
	NewIntent.MergedViewTarget = ViewTarget;
	NewIntent.BlendTime = FMath::Max(BlendTime, 0.0f);
	NewIntent.Easing = Easing;

	SetViewIntentOnAuthority(NewIntent);
}

void AUSViewState::ClearMerged(float Player1Share, float BlendTime, EUSViewEasing Easing)
{
	FUSViewIntent NewIntent = ViewIntent;
	NewIntent.bMerged = false;
	NewIntent.MergedViewTarget = nullptr;
	NewIntent.Player1Share = FMath::Clamp(Player1Share, 0.0f, 1.0f);
	NewIntent.BlendTime = FMath::Max(BlendTime, 0.0f);
	NewIntent.Easing = Easing;

	SetViewIntentOnAuthority(NewIntent);
}

void AUSViewState::SetViewIntentOnAuthority(const FUSViewIntent& NewIntent)
{
	if (!HasAuthority())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("ViewState: refusing a layout change on a non-authority machine. Layout is server-authoritative; there is no client request path."));
		return;
	}

	ViewIntent = NewIntent;

	// Load bearing — see the comment on the declaration. Without this the host's
	// own screen never moves, and in standalone the host is the only screen.
	OnRep_ViewIntent();
}

void AUSViewState::OnRep_ViewIntent()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ViewState: intent Player1Share=%.3f bMerged=%d MergedViewTarget=%s BlendTime=%.3f Easing=%s (Authority=%d)"),
		ViewIntent.Player1Share,
		ViewIntent.bMerged ? 1 : 0,
		*GetNameSafe(ViewIntent.MergedViewTarget.Get()),
		ViewIntent.BlendTime,
		*UEnum::GetValueAsString(ViewIntent.Easing),
		HasAuthority() ? 1 : 0);

	UGameInstance* GameInstance = GetGameInstance();
	if (UUSViewSubsystem* ViewSubsystem = GameInstance != nullptr ? GameInstance->GetSubsystem<UUSViewSubsystem>() : nullptr)
	{
		ViewSubsystem->ApplyViewIntent(ViewIntent);
	}
}
