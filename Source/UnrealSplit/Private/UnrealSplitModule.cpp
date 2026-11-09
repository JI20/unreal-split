// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "UnrealSplitModule.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

#include "USRemoteViewSubsystem.h"
#include "USSessionSubsystem.h"

#define LOCTEXT_NAMESPACE "FUnrealSplitModule"

DEFINE_LOG_CATEGORY(LogUnrealSplit);

void FUnrealSplitModule::StartupModule()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("UnrealSplit module started."));
}

void FUnrealSplitModule::ShutdownModule()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("UnrealSplit module shut down."));
}

void UnrealSplit::NotifySeamlessTravelPlayer(APlayerController* PlayerController)
{
	const UWorld* const World = PlayerController != nullptr ? PlayerController->GetWorld() : nullptr;
	UGameInstance* const GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;

	if (GameInstance == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("NotifySeamlessTravelPlayer was called without a player controller in a world; camera streams have not been rebuilt."));
		return;
	}

	// Streams first. Registration is a reconcile that usually finds nothing to do,
	// while a missing stream is the visible failure, so it is the one that should
	// not wait behind anything.
	if (UUSRemoteViewSubsystem* const RemoteView = GameInstance->GetSubsystem<UUSRemoteViewSubsystem>())
	{
		RemoteView->HandleSeamlessTravelPlayer(PlayerController);
	}

	if (UUSSessionSubsystem* const Sessions = GameInstance->GetSubsystem<UUSSessionSubsystem>())
	{
		Sessions->ReregisterPlayersAfterSeamlessTravel();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealSplitModule, UnrealSplit)
