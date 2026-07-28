// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USViewState.h"

#include "Engine/GameInstance.h"
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

void AUSViewState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUSViewState, ViewIntent);
}

void AUSViewState::SetPerspective(EUSPerspective Perspective, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ViewState::SetPerspective: Perspective=%s BlendTime=%.3f Authority=%d"),
		*UEnum::GetValueAsString(Perspective), BlendTime, HasAuthority() ? 1 : 0);
}

void AUSViewState::SetSplitRatio(float Player1Share, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ViewState::SetSplitRatio: Player1Share=%.3f BlendTime=%.3f Authority=%d"),
		Player1Share, BlendTime, HasAuthority() ? 1 : 0);
}

void AUSViewState::SetMerged(EUSMergedCameraPolicy Policy, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ViewState::SetMerged: Policy=%s BlendTime=%.3f Authority=%d"),
		*UEnum::GetValueAsString(Policy), BlendTime, HasAuthority() ? 1 : 0);
}

void AUSViewState::ClearMerged(float Player1Share, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ViewState::ClearMerged: Player1Share=%.3f BlendTime=%.3f Authority=%d"),
		Player1Share, BlendTime, HasAuthority() ? 1 : 0);
}

void AUSViewState::OnRep_ViewIntent()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ViewState::OnRep_ViewIntent: Perspective=%s Player1Share=%.3f bMerged=%d Policy=%s BlendTime=%.3f"),
		*UEnum::GetValueAsString(ViewIntent.Perspective),
		ViewIntent.Player1Share,
		ViewIntent.bMerged ? 1 : 0,
		*UEnum::GetValueAsString(ViewIntent.MergedCameraPolicy),
		ViewIntent.BlendTime);

	UGameInstance* GameInstance = GetGameInstance();
	if (UUSViewSubsystem* ViewSubsystem = GameInstance ? GameInstance->GetSubsystem<UUSViewSubsystem>() : nullptr)
	{
		ViewSubsystem->ApplyViewIntent(ViewIntent);
	}
}
