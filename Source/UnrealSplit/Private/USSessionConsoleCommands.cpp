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

#include "USConsoleReport.h"
#include "USSessionSubsystem.h"

// Development-only driver for the session layer until real menu UI exists.
// The console is compiled out of shipping builds, so these go with it.
#if !UE_BUILD_SHIPPING

using USConsole::Report;
using USConsole::ReportWarning;

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSHostCommand(
	TEXT("US.Host"),
	TEXT("UnrealSplit: create a session and reopen the current level as a listen server. Usage: US.Host [MaxPlayers] (defaults to 2). Counts every player in the game, local and remote, and is clamped to the game's total player cap."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar))
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
		if (UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar))
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
		if (UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar))
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
		if (UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar))
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
		if (UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar))
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
		if (UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar))
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

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSConnectionModeCommand(
	TEXT("US.ConnectionMode"),
	TEXT("UnrealSplit: show or change how players connect. Usage: US.ConnectionMode [LAN|Internet]. Without an argument it only reports. Only allowed while idle, and it starts the backend login the new mode needs."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		if (Args.Num() == 0)
		{
			Report(Ar, FString::Printf(TEXT("US.ConnectionMode: %s, backend '%s' (%s, %s)."),
				*UEnum::GetValueAsString(Sessions->GetConnectionMode()),
				*Sessions->GetActiveBackendName().ToString(),
				Sessions->IsBackendInstalled() ? TEXT("installed") : TEXT("NOT installed in this build"),
				Sessions->IsBackendReady() ? TEXT("ready") : TEXT("no logged-in user yet")));
			return;
		}

		EUSConnectionMode NewMode = EUSConnectionMode::LAN;
		if (Args[0].Equals(TEXT("Internet"), ESearchCase::IgnoreCase))
		{
			NewMode = EUSConnectionMode::Internet;
		}
		else if (!Args[0].Equals(TEXT("LAN"), ESearchCase::IgnoreCase))
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.ConnectionMode: '%s' is not a mode. Usage: US.ConnectionMode [LAN|Internet]."), *Args[0]));
			return;
		}

		if (Sessions->SetConnectionMode(NewMode))
		{
			Report(Ar, FString::Printf(TEXT("US.ConnectionMode: now %s on backend '%s' (%s). A login that is still running reports in the log."),
				*UEnum::GetValueAsString(Sessions->GetConnectionMode()),
				*Sessions->GetActiveBackendName().ToString(),
				Sessions->IsBackendReady() ? TEXT("ready") : TEXT("waiting for login")));
		}
		else
		{
			// The subsystem's own line says whether it was the state or the backend.
			ReportWarning(Ar, FString::Printf(TEXT("US.ConnectionMode: refused; still %s."), *UEnum::GetValueAsString(Sessions->GetConnectionMode())));
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSLoginCommand(
	TEXT("US.Login"),
	TEXT("UnrealSplit: log the primary user into the current mode's backend. Only needed as a retry: changing mode already does it, and LAN backends generally need no login at all."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		if (Sessions->IsBackendReady())
		{
			Report(Ar, FString::Printf(TEXT("US.Login: backend '%s' is already usable; nothing to do."), *Sessions->GetActiveBackendName().ToString()));
			return;
		}

		if (Sessions->LoginToBackend())
		{
			Report(Ar, FString::Printf(TEXT("US.Login: logging in to '%s'; the outcome follows in the log."), *Sessions->GetActiveBackendName().ToString()));
		}
		else
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.Login: could not start a login on '%s'."), *Sessions->GetActiveBackendName().ToString()));
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSSessionStateCommand(
	TEXT("US.SessionState"),
	TEXT("UnrealSplit: report session state, the online backend in use, this machine's local players and the cached search results."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		const UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		// Naming the backend rather than leaving it to be inferred: with two of
		// them installed, "which one am I actually talking to" is otherwise only
		// answerable from the startup log. The instance name distinguishes PIE
		// worlds, which each get their own.
		const FName BackendName = Sessions->GetActiveBackendName();
		const IOnlineSubsystem* Backend = Online::GetSubsystem(World, BackendName);
		const IOnlineSessionPtr SessionInterface = Online::GetSessionInterface(World, BackendName);

		Report(Ar, FString::Printf(TEXT("US.SessionState: State=%s Mode=%s Backend=%s Instance=%s SessionInterface=%s Ready=%s"),
			*UEnum::GetValueAsString(Sessions->GetSessionState()),
			*UEnum::GetValueAsString(Sessions->GetConnectionMode()),
			Backend != nullptr ? *Backend->GetSubsystemName().ToString() : *FString::Printf(TEXT("%s (not installed)"), *BackendName.ToString()),
			Backend != nullptr ? *Backend->GetInstanceName().ToString() : TEXT("none"),
			SessionInterface.IsValid() ? TEXT("yes") : TEXT("no"),
			Sessions->IsBackendReady() ? TEXT("yes") : TEXT("no (login pending)")));

		// The process default is a different thing from the selected backend and
		// only engine-internal code uses it; worth showing side by side, because a
		// mismatch is exactly what makes the engine's own player registration go
		// to a backend with no session on it.
		const IOnlineSubsystem* DefaultBackend = Online::GetSubsystem(World);
		Report(Ar, FString::Printf(TEXT("  Process default backend: %s (engine-internal calls only)."),
			DefaultBackend != nullptr ? *DefaultBackend->GetSubsystemName().ToString() : TEXT("none")));

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
		const IOnlineIdentityPtr Identity = Online::GetIdentityInterface(World, BackendName);
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
