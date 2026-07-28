// Copyright (c) ji20. All Rights Reserved.

#include "USViewSubsystem.h"

#include "Engine/World.h"
#include "UObject/Class.h"

#include "USViewState.h"
#include "UnrealSplitModule.h"

void UUSViewSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	WorldChangedHandle = FWorldDelegates::OnGameInstanceWorldChanged.AddUObject(this, &UUSViewSubsystem::HandleGameInstanceWorldChanged);

	UE_LOG(LogUnrealSplit, Log, TEXT("View subsystem initialized."));
}

void UUSViewSubsystem::Deinitialize()
{
	FWorldDelegates::OnGameInstanceWorldChanged.Remove(WorldChangedHandle);
	WorldChangedHandle.Reset();

	UE_LOG(LogUnrealSplit, Log, TEXT("View subsystem deinitialized."));

	Super::Deinitialize();
}

void UUSViewSubsystem::SetSplitRatio(float Player1Share, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("SetSplitRatio: Player1Share=%.3f BlendTime=%.3f"), Player1Share, BlendTime);
}

void UUSViewSubsystem::SplitViews(float Player1Share, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("SplitViews: Player1Share=%.3f BlendTime=%.3f"), Player1Share, BlendTime);
}

void UUSViewSubsystem::MergeViews(float BlendTime, EUSMergedCameraPolicy Policy)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("MergeViews: BlendTime=%.3f Policy=%s"), BlendTime, *UEnum::GetValueAsString(Policy));
}

void UUSViewSubsystem::SetCameraPerspective(EUSPerspective Perspective, float BlendTime)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("SetCameraPerspective: Perspective=%s BlendTime=%.3f"), *UEnum::GetValueAsString(Perspective), BlendTime);
}

AUSViewState* UUSViewSubsystem::GetViewState() const
{
	return nullptr;
}

void UUSViewSubsystem::ApplyViewIntent(const FUSViewIntent& Intent)
{
	UE_LOG(LogUnrealSplit, Log, TEXT("ApplyViewIntent: Perspective=%s Player1Share=%.3f bMerged=%d Policy=%s BlendTime=%.3f"),
		*UEnum::GetValueAsString(Intent.Perspective),
		Intent.Player1Share,
		Intent.bMerged ? 1 : 0,
		*UEnum::GetValueAsString(Intent.MergedCameraPolicy),
		Intent.BlendTime);
}

void UUSViewSubsystem::HandleGameInstanceWorldChanged(UGameInstance* OwningGameInstance, UWorld* OldWorld, UWorld* NewWorld)
{
	if (OwningGameInstance != GetGameInstance())
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("World changed: Old=%s New=%s"),
		OldWorld ? *OldWorld->GetName() : TEXT("None"),
		NewWorld ? *NewWorld->GetName() : TEXT("None"));
}
