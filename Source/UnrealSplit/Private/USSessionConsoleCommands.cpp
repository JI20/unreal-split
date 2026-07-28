// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Class.h"

#include "USSessionSubsystem.h"
#include "UnrealSplitModule.h"

// Development-only driver for the session layer until real menu UI exists.
// The console is compiled out of shipping builds, so these go with it.
#if !UE_BUILD_SHIPPING

namespace
{
	UUSSessionSubsystem* FindSessionSubsystem(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UUSSessionSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UUSSessionSubsystem>() : nullptr;

		if (Subsystem == nullptr)
		{
			UE_LOG(LogUnrealSplit, Warning, TEXT("No UnrealSplit session subsystem in this world. Run this from a running game or PIE session."));
		}

		return Subsystem;
	}
}

static FAutoConsoleCommandWithWorld GUSHostCommand(
	TEXT("US.Host"),
	TEXT("UnrealSplit: create a session and reopen the current level as a listen server."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World))
		{
			Sessions->HostSession();
		}
	}));

static FAutoConsoleCommandWithWorld GUSFindCommand(
	TEXT("US.Find"),
	TEXT("UnrealSplit: search for joinable sessions. Results are logged when the search completes."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World))
		{
			Sessions->FindSessions();
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GUSJoinCommand(
	TEXT("US.Join"),
	TEXT("UnrealSplit: join a session found by US.Find. Usage: US.Join [Index] (defaults to 0)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World))
		{
			const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
			Sessions->JoinFoundSession(Index);
		}
	}));

static FAutoConsoleCommandWithWorld GUSLeaveCommand(
	TEXT("US.Leave"),
	TEXT("UnrealSplit: destroy the current session and return to the entry map."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World))
		{
			Sessions->LeaveSession();
		}
	}));

static FAutoConsoleCommandWithWorld GUSSessionStateCommand(
	TEXT("US.SessionState"),
	TEXT("UnrealSplit: log the current session state and cached search results."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (const UUSSessionSubsystem* Sessions = FindSessionSubsystem(World))
		{
			const TArray<FUSSessionSearchResultInfo> Found = Sessions->GetFoundSessions();

			UE_LOG(LogUnrealSplit, Log, TEXT("Session state: %s (%d cached search result(s))."),
				*UEnum::GetValueAsString(Sessions->GetSessionState()), Found.Num());

			for (int32 Index = 0; Index < Found.Num(); ++Index)
			{
				UE_LOG(LogUnrealSplit, Log, TEXT("  [%d] Owner='%s' Ping=%dms Slots=%d/%d"),
					Index, *Found[Index].OwnerName, Found[Index].PingInMs,
					Found[Index].OpenPublicConnections, Found[Index].MaxPublicConnections);
			}
		}
	}));

#endif // !UE_BUILD_SHIPPING
