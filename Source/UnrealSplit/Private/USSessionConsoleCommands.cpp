// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"

#include "Engine/Engine.h"
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
#include "USRemoteViewSubsystem.h"
#include "USSessionSubsystem.h"
#include "USShadowLocalPlayer.h"

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
		UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		// Same reason as US.Find: a host that was never created is what the other
		// machine later experiences as "no sessions found", one process away from
		// where the problem actually is.
		const EUSBackendStatus Status = Sessions->GetBackendStatus();
		if (Status != EUSBackendStatus::Ready)
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.Host: NOT HOSTING. Backend '%s' is %s. Sign in first (US.Login), then US.Host again."),
				*Sessions->GetBackendName().ToString(), *UEnum::GetValueAsString(Status)));

			Sessions->LoginToBackend();
			return;
		}

		const int32 MaxPlayers = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 2;
		Report(Ar, FString::Printf(TEXT("US.Host: creating a session for up to %d player(s) on '%s' (%s); the outcome follows in the log."),
			MaxPlayers, *Sessions->GetBackendName().ToString(), *Sessions->DescribeDiscoveryKeys()));
		Sessions->HostSession(MaxPlayers);
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSFindCommand(
	TEXT("US.Find"),
	TEXT("UnrealSplit: search for joinable sessions. Run US.SessionState afterwards to list what was found."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		// Answered before the search rather than after it, because a search that
		// never ran and a search that found nothing both end in an empty list.
		// The subsystem would refuse this anyway; saying so here is what puts the
		// reason on screen next to the command that asked for it.
		const EUSBackendStatus Status = Sessions->GetBackendStatus();
		if (Status != EUSBackendStatus::Ready)
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.Find: NOT SEARCHING. Backend '%s' is %s, so no search ran at all. This is not an empty result. %s"),
				*Sessions->GetBackendName().ToString(),
				*UEnum::GetValueAsString(Status),
				Status == EUSBackendStatus::NotInstalled
					? TEXT("No online subsystem came up for this process's mode.")
					: TEXT("Sign in first: US.Login, then US.Find again. On EOS the account must be served by the Developer Authentication Tool and belong to the organisation that owns the application.")));

			// Started rather than merely suggested, so the retry has something to
			// wait for; the outcome lands in the log.
			Sessions->LoginToBackend();
			return;
		}

		Report(Ar, FString::Printf(TEXT("US.Find: searching on '%s' (%s); the results print here when the search resolves, and US.SessionState lists them again later."),
			*Sessions->GetBackendName().ToString(), *Sessions->DescribeDiscoveryKeys()));
		Sessions->FindSessions();
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

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSTravelCommand(
	TEXT("US.Travel"),
	TEXT("UnrealSplit: take everyone in this session to another map, keeping the session and every local player. Usage: US.Travel <MapName>. Host or standalone only; couch co-op has no session and travels the same way."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSSessionSubsystem* const Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		if (Args.Num() < 1)
		{
			ReportWarning(Ar, TEXT("US.Travel: needs a destination. Usage: US.Travel <MapName>, e.g. US.Travel /Game/Maps/L_Game."));
			return;
		}

		const FString& MapName = Args[0];

		if (Sessions->TravelSessionToMap(MapName))
		{
			Report(Ar, FString::Printf(TEXT("US.Travel: travelling to '%s'."), *MapName));
		}
		else
		{
			// The subsystem's own line says which refusal it was.
			ReportWarning(Ar, FString::Printf(TEXT("US.Travel: could not travel to '%s'."), *MapName));
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
			const int32 NumLocalPlayers = UUSShadowLocalPlayer::CountRealLocalPlayers(World->GetGameInstance());

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
			const int32 NumLocalPlayers = UUSShadowLocalPlayer::CountRealLocalPlayers(World->GetGameInstance());

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

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSLoginCommand(
	TEXT("US.Login"),
	TEXT("UnrealSplit: log the primary user into this process's backend. Only needed as a retry: the first session call that needs a login starts one, and LAN backends generally need no login at all."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		if (Sessions->IsBackendReady())
		{
			Report(Ar, FString::Printf(TEXT("US.Login: backend '%s' is already usable; nothing to do."), *Sessions->GetBackendName().ToString()));
			return;
		}

		if (Sessions->LoginToBackend())
		{
			Report(Ar, FString::Printf(TEXT("US.Login: logging in to '%s'; the outcome follows in the log."), *Sessions->GetBackendName().ToString()));
		}
		else
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.Login: could not start a login on '%s'."), *Sessions->GetBackendName().ToString()));
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSSessionStateCommand(
	TEXT("US.SessionState"),
	TEXT("UnrealSplit: report session state, the connection mode and backend this process was launched on, this machine's local players and the cached search results."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		const UUSSessionSubsystem* Sessions = USConsole::FindSubsystem<UUSSessionSubsystem>(World, Ar);
		if (Sessions == nullptr)
		{
			return;
		}

		// The mode and the backend together, because the mode is only a name for
		// which backend the process was launched on: it is fixed at startup by
		// -ConnectionMode= and there is no way to change it from here. The
		// instance name distinguishes PIE worlds, which each get their own.
		const IOnlineSubsystem* Backend = Online::GetSubsystem(World);
		const IOnlineSessionPtr SessionInterface = Online::GetSessionInterface(World);

		Report(Ar, FString::Printf(TEXT("US.SessionState: State=%s Mode=%s (fixed at launch) Backend=%s Instance=%s SessionInterface=%s Status=%s"),
			*UEnum::GetValueAsString(Sessions->GetSessionState()),
			*UEnum::GetValueAsString(Sessions->GetConnectionMode()),
			Backend != nullptr ? *Backend->GetSubsystemName().ToString() : *FString::Printf(TEXT("%s (none came up)"), *Sessions->GetBackendName().ToString()),
			Backend != nullptr ? *Backend->GetInstanceName().ToString() : TEXT("none"),
			SessionInterface.IsValid() ? TEXT("yes") : TEXT("no"),
			*UEnum::GetValueAsString(Sessions->GetBackendStatus())));

		// The keys a host and a searcher have to agree on, printed on both
		// machines so a discovery failure is settled by comparing two lines
		// rather than by reasoning about which side is at fault.
		Report(Ar, FString::Printf(TEXT("  Discovery: %s"), *Sessions->DescribeDiscoveryKeys()));

		const FNamedOnlineSession* Session = SessionInterface.IsValid() ? SessionInterface->GetNamedSession(NAME_GameSession) : nullptr;
		if (Session != nullptr)
		{
			const FOnlineSessionSettings& Settings = Session->SessionSettings;

			Report(Ar, FString::Printf(TEXT("  Session '%s' (%s): hosting=%s, %d registered player(s), %d of %d public connection(s) free."),
				*FName(NAME_GameSession).ToString(),
				EOnlineSessionState::ToString(Session->SessionState),
				Session->bHosting ? TEXT("yes") : TEXT("no"),
				Session->RegisteredPlayers.Num(),
				Session->NumOpenPublicConnections,
				Settings.NumPublicConnections));

			// As the backend actually holds them, not as HostSession asked for
			// them: the EOS layer rewrites some of these on create, and it is the
			// rewritten values a searcher has to match.
			Report(Ar, FString::Printf(TEXT("    Advertise=%d LANMatch=%d Lobbies=%d Presence=%d JoinInProgress=%d JoinViaPresence=%d BuildUniqueId=%d Id='%s'"),
				Settings.bShouldAdvertise ? 1 : 0,
				Settings.bIsLANMatch ? 1 : 0,
				Settings.bUseLobbiesIfAvailable ? 1 : 0,
				Settings.bUsesPresence ? 1 : 0,
				Settings.bAllowJoinInProgress ? 1 : 0,
				Settings.bAllowJoinViaPresence ? 1 : 0,
				Settings.BuildUniqueId,
				*Session->GetSessionIdStr()));
		}
		else
		{
			Report(Ar, TEXT("  No session on this backend: local co-op, or nothing hosted or joined yet."));
		}

		// Login status per local player, because a player without an online
		// identity can never be registered in the session, and that is invisible
		// otherwise until registration silently skips it.
		const IOnlineIdentityPtr Identity = Online::GetIdentityInterface(World);

		// People, not views. A shadow view rendering the remote partner is a local
		// player as far as the renderer is concerned and nothing at all as far as
		// the session is concerned, so listing it here would invite exactly the
		// wrong conclusion about who is registered and who holds a seat.
		TArray<ULocalPlayer*> LocalPlayers;
		UUSShadowLocalPlayer::GetRealLocalPlayers(World->GetGameInstance(), LocalPlayers);
		Report(Ar, FString::Printf(TEXT("  %d local player(s) on this machine:"), LocalPlayers.Num()));

		for (int32 Index = 0; Index < LocalPlayers.Num(); ++Index)
		{
			const ULocalPlayer* LocalPlayer = LocalPlayers[Index];
			const FUniqueNetIdRepl PlayerId = LocalPlayer != nullptr ? LocalPlayer->GetPreferredUniqueNetId() : FUniqueNetIdRepl();

			// Asked by user index rather than by id: the id overloads downcast
			// their argument to the backend's own type, so a user index is the one
			// question that cannot go wrong.
			const int32 UserIndex = LocalPlayer != nullptr ? LocalPlayer->GetPlatformUserIndex() : INDEX_NONE;
			const ELoginStatus::Type LoginStatus = (Identity.IsValid() && UserIndex >= 0)
				? Identity->GetLoginStatus(UserIndex)
				: ELoginStatus::NotLoggedIn;

			Report(Ar, FString::Printf(TEXT("    [%d] PlatformUser=%d UserIndex=%d Login=%s Id='%s'"),
				Index,
				LocalPlayer != nullptr ? LocalPlayer->GetPlatformUserId().GetInternalId() : INDEX_NONE,
				UserIndex,
				ELoginStatus::ToString(LoginStatus),
				PlayerId.IsValid() ? *PlayerId.ToDebugString() : TEXT("invalid")));
		}

		// Named but not listed as a player, so a second view on screen is never a
		// mystery here while still not counting as somebody in the session.
		if (const UUSRemoteViewSubsystem* RemoteView = World->GetGameInstance()->GetSubsystem<UUSRemoteViewSubsystem>())
		{
			if (RemoteView->IsShadowViewActive())
			{
				Report(Ar, TEXT("    plus a shadow view drawing the remote partner, which is not a player here — see US.ViewState."));
			}
		}

		const TArray<FUSSessionSearchResultInfo> Found = Sessions->GetFoundSessions();
		Report(Ar, FString::Printf(TEXT("  %d cached search result(s):"), Found.Num()));

		for (int32 Index = 0; Index < Found.Num(); ++Index)
		{
			Report(Ar, FString::Printf(TEXT("    [%d] Id='%s' Owner='%s' Ping=%dms Slots=%d/%d"),
				Index, *Found[Index].SessionId, *Found[Index].OwnerName, Found[Index].PingInMs,
				Found[Index].OpenPublicConnections, Found[Index].MaxPublicConnections));
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSNetProfileCommand(
	TEXT("US.NetProfile"),
	TEXT("UnrealSplit: apply or clear a named packet-simulation profile (opt-in). Usage: US.NetProfile [clean|good|average|poor|torture|off]. Does nothing until you run it — profiles in DefaultEngine.ini are never auto-applied. Compiles out of Shipping (DO_ENABLE_NET_TEST)."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* const ProfileNames[] = {
			TEXT("clean"), TEXT("good"), TEXT("average"), TEXT("poor"), TEXT("torture")
		};

		if (World == nullptr)
		{
			ReportWarning(Ar, TEXT("US.NetProfile: no world."));
			return;
		}

		if (Args.Num() == 0)
		{
			Report(Ar, TEXT("US.NetProfile: opt-in only. Usage: US.NetProfile <clean|good|average|poor|torture|off>. Nothing is applied at launch; 'off' clears emulation."));
			return;
		}

		const FString& Arg = Args[0];
		if (Arg.Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			// Clears PersistentPacketSimulationSettings and pushes zeros onto
			// active net drivers. Safe to call when nothing was applied.
			GEngine->Exec(World, TEXT("NetEmulation.Off"), Ar);
			Report(Ar, TEXT("US.NetProfile: network emulation off."));
			return;
		}

		bool bKnown = false;
		for (const TCHAR* Name : ProfileNames)
		{
			if (Arg.Equals(Name, ESearchCase::IgnoreCase))
			{
				bKnown = true;
				break;
			}
		}

		if (!bKnown)
		{
			ReportWarning(Ar, FString::Printf(
				TEXT("US.NetProfile: unknown profile '%s'. Use clean, good, average, poor, torture, or off."), *Arg));
			return;
		}

		// Engine loads [PacketSimulationProfile.<Name>] from GEngineIni and
		// applies it. Missing section → engine logs and leaves settings unchanged.
		const FString Cmd = FString::Printf(TEXT("NetEmulation.PktEmulationProfile %s"), *Arg);
		GEngine->Exec(World, *Cmd, Ar);
		Report(Ar, FString::Printf(
			TEXT("US.NetProfile: applied '%s'. Affects remote NetConnections only (Null and EOS). Couch / standalone unchanged. US.NetProfile off to clear."),
			*Arg));
	}));

#endif // !UE_BUILD_SHIPPING
