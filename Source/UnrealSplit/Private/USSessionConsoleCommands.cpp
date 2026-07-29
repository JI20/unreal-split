// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"

#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/OnlineReplStructs.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "UObject/Class.h"

#include "USSessionSubsystem.h"
#include "UnrealSplitModule.h"

// Development-only driver for the session layer until real menu UI exists.
// The console is compiled out of shipping builds, so these go with it.
#if !UE_BUILD_SHIPPING

namespace
{
	/**
	 * Answers through the command's output device *and* the log. The in-game
	 * console only echoes log lines that pass con.MinLogVerbosity — nothing, by
	 * default — so without the output device these answers are invisible unless
	 * the game was launched with -log, while the log is what survives the
	 * session. Asynchronous outcomes arrive long after the command returned and
	 * cannot use this; they stay log-only.
	 */
	void Report(FOutputDevice& Ar, const FString& Message)
	{
		Ar.Log(Message);
		UE_LOG(LogUnrealSplit, Log, TEXT("%s"), *Message);
	}

	void ReportWarning(FOutputDevice& Ar, const FString& Message)
	{
		Ar.Log(ELogVerbosity::Warning, Message);
		UE_LOG(LogUnrealSplit, Warning, TEXT("%s"), *Message);
	}

	UUSSessionSubsystem* FindSessionSubsystem(UWorld* World, FOutputDevice& Ar)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UUSSessionSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UUSSessionSubsystem>() : nullptr;

		if (Subsystem == nullptr)
		{
			ReportWarning(Ar, TEXT("No UnrealSplit session subsystem in this world. Run this from a running game or PIE session."));
		}

		return Subsystem;
	}
}

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSHostCommand(
	TEXT("US.Host"),
	TEXT("UnrealSplit: create a session and reopen the current level as a listen server. Usage: US.Host [MaxPlayers] (defaults to 2). Counts every player in the game, local and remote, and is clamped to the game's total player cap."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar))
		{
			const int32 MaxPlayers = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 2;
			Report(Ar, FString::Printf(TEXT("US.Host: creating a session for up to %d player(s); the outcome follows in the log."), MaxPlayers));
			Sessions->HostSession(MaxPlayers);
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSFindCommand(
	TEXT("US.Find"),
	TEXT("UnrealSplit: search for joinable sessions. Run US.SessionState afterwards to list what was found."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar))
		{
			Report(Ar, TEXT("US.Find: searching; results are logged on completion and listed by US.SessionState."));
			Sessions->FindSessions();
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSJoinCommand(
	TEXT("US.Join"),
	TEXT("UnrealSplit: join a session found by US.Find. Usage: US.Join [Index] (defaults to 0)."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar))
		{
			const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
			Report(Ar, FString::Printf(TEXT("US.Join: joining search result %d; the outcome follows in the log."), Index));
			Sessions->JoinFoundSession(Index);
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSLeaveCommand(
	TEXT("US.Leave"),
	TEXT("UnrealSplit: destroy the current session and return to the entry map."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar))
		{
			Report(Ar, TEXT("US.Leave: destroying the session and returning to the entry map."));
			Sessions->LeaveSession();
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSAddLocalPlayerCommand(
	TEXT("US.AddLocalPlayer"),
	TEXT("UnrealSplit: add a couch co-op player on this machine (vertical splitscreen). On a joined client this asks the host for a split join, which needs a free public connection there."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar))
		{
			const bool bAdded = Sessions->AddLocalCoopPlayer();
			const int32 NumLocalPlayers = World->GetGameInstance()->GetNumLocalPlayers();

			if (bAdded)
			{
				Report(Ar, FString::Printf(TEXT("US.AddLocalPlayer: added; %d local player(s) on this machine now."), NumLocalPlayers));
			}
			else
			{
				// The subsystem's own line says which of the two it was.
				ReportWarning(Ar, FString::Printf(TEXT("US.AddLocalPlayer: refused, or waiting on an online login that has not resolved yet; still %d local player(s)."), NumLocalPlayers));
			}
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSRemoveLocalPlayerCommand(
	TEXT("US.RemoveLocalPlayer"),
	TEXT("UnrealSplit: remove the most recently added couch co-op player."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar))
		{
			const bool bRemoved = Sessions->RemoveLocalCoopPlayer();
			const int32 NumLocalPlayers = World->GetGameInstance()->GetNumLocalPlayers();

			if (bRemoved)
			{
				Report(Ar, FString::Printf(TEXT("US.RemoveLocalPlayer: removed; %d local player(s) on this machine now."), NumLocalPlayers));
			}
			else
			{
				ReportWarning(Ar, FString::Printf(TEXT("US.RemoveLocalPlayer: refused; still %d local player(s)."), NumLocalPlayers));
			}
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSSessionStateCommand(
	TEXT("US.SessionState"),
	TEXT("UnrealSplit: report session state, the online backend in use, this machine's local players and the cached search results."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		const UUSSessionSubsystem* Sessions = FindSessionSubsystem(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		// Naming the backend rather than leaving it to be inferred: with two of
		// them installed, "which one am I actually talking to" is otherwise only
		// answerable from the startup log. The instance name distinguishes PIE
		// worlds, which each get their own.
		const IOnlineSubsystem* Backend = Online::GetSubsystem(World);
		const IOnlineSessionPtr SessionInterface = Online::GetSessionInterface(World);

		Report(Ar, FString::Printf(TEXT("US.SessionState: State=%s Backend=%s Instance=%s SessionInterface=%s"),
			*UEnum::GetValueAsString(Sessions->GetSessionState()),
			Backend != nullptr ? *Backend->GetSubsystemName().ToString() : TEXT("none"),
			Backend != nullptr ? *Backend->GetInstanceName().ToString() : TEXT("none"),
			SessionInterface.IsValid() ? TEXT("yes") : TEXT("no")));

		const FNamedOnlineSession* Session = SessionInterface.IsValid() ? SessionInterface->GetNamedSession(NAME_GameSession) : nullptr;
		if (Session != nullptr)
		{
			Report(Ar, FString::Printf(TEXT("  Session '%s': hosting=%s, %d registered player(s), %d of %d public connection(s) free."),
				*FName(NAME_GameSession).ToString(),
				Session->bHosting ? TEXT("yes") : TEXT("no"),
				Session->RegisteredPlayers.Num(),
				Session->NumOpenPublicConnections,
				Session->SessionSettings.NumPublicConnections));
		}
		else
		{
			Report(Ar, TEXT("  No session on this backend: local co-op, or nothing hosted or joined yet."));
		}

		// Login status per local player, because a player without an online
		// identity can never be registered in the session, and that is invisible
		// otherwise until registration silently skips it.
		const IOnlineIdentityPtr Identity = Online::GetIdentityInterface(World);
		const TArray<ULocalPlayer*>& LocalPlayers = World->GetGameInstance()->GetLocalPlayers();
		Report(Ar, FString::Printf(TEXT("  %d local player(s) on this machine:"), LocalPlayers.Num()));

		for (int32 Index = 0; Index < LocalPlayers.Num(); ++Index)
		{
			const ULocalPlayer* LocalPlayer = LocalPlayers[Index];
			const FUniqueNetIdRepl PlayerId = LocalPlayer != nullptr ? LocalPlayer->GetPreferredUniqueNetId() : FUniqueNetIdRepl();
			const ELoginStatus::Type LoginStatus = (Identity.IsValid() && PlayerId.IsValid())
				? Identity->GetLoginStatus(*PlayerId)
				: ELoginStatus::NotLoggedIn;

			Report(Ar, FString::Printf(TEXT("    [%d] PlatformUser=%d Login=%s Id='%s'"),
				Index,
				LocalPlayer != nullptr ? LocalPlayer->GetPlatformUserId().GetInternalId() : INDEX_NONE,
				ELoginStatus::ToString(LoginStatus),
				PlayerId.IsValid() ? *PlayerId.ToDebugString() : TEXT("invalid")));
		}

		const TArray<FUSSessionSearchResultInfo> Found = Sessions->GetFoundSessions();
		Report(Ar, FString::Printf(TEXT("  %d cached search result(s):"), Found.Num()));

		for (int32 Index = 0; Index < Found.Num(); ++Index)
		{
			Report(Ar, FString::Printf(TEXT("    [%d] Owner='%s' Ping=%dms Slots=%d/%d"),
				Index, *Found[Index].OwnerName, Found[Index].PingInMs,
				Found[Index].OpenPublicConnections, Found[Index].MaxPublicConnections));
		}
	}));

#endif // !UE_BUILD_SHIPPING
