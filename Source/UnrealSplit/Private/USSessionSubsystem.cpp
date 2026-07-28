// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USSessionSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameMapsSettings.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystemUtils.h"
#include "UObject/Class.h"

#include "UnrealSplitModule.h"

namespace
{
	/** Local player index driving session calls; two-player co-op only ever hosts from player 0. */
	constexpr int32 SessionLocalUserNum = 0;

	/** A LAN co-op lobby list never needs to be long. */
	constexpr int32 SessionMaxSearchResults = 20;

	/** Backends that honour a search timeout give up after this long. */
	constexpr float SessionSearchTimeoutSeconds = 15.0f;

	/** Resolves the session interface of whichever backend config selected. */
	IOnlineSessionPtr GetSessionInterface(const UGameInstance* GameInstance)
	{
		const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
		return World ? Online::GetSessionInterface(World) : nullptr;
	}
}

void UUSSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &UUSSessionSubsystem::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &UUSSessionSubsystem::HandleTravelFailure);
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Session subsystem initialized. LANMatch=%d"), bUseLANMatch ? 1 : 0);
}

void UUSSessionSubsystem::Deinitialize()
{
	ClearSessionDelegates();

	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	NetworkFailureHandle.Reset();
	TravelFailureHandle.Reset();

	SessionSearch.Reset();
	FoundSessions.Reset();

	UE_LOG(LogUnrealSplit, Log, TEXT("Session subsystem deinitialized."));

	Super::Deinitialize();
}

void UUSSessionSubsystem::HostSession(int32 MaxPlayers)
{
	if (SessionState != EUSSessionState::Idle)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("HostSession rejected: state is %s, expected Idle."), *UEnum::GetValueAsString(SessionState));
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance());
	if (!Sessions.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("HostSession failed: no session interface. Check DefaultPlatformService in DefaultEngine.ini."));
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	if (Sessions->GetNamedSession(NAME_GameSession) != nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("HostSession rejected: session '%s' already exists. Call LeaveSession first."), *FName(NAME_GameSession).ToString());
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections = FMath::Max(1, MaxPlayers);
	Settings.NumPrivateConnections = 0;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	Settings.bIsLANMatch = bUseLANMatch;
	Settings.bIsDedicated = false;
	Settings.bUsesPresence = false;
	Settings.bAllowJoinViaPresence = false;
	Settings.bAllowJoinViaPresenceFriendsOnly = false;
	Settings.bAllowInvites = false;
	// Lobbies are an internet-backend feature; revisit together with EOS.
	Settings.bUseLobbiesIfAvailable = false;

	const uint32 OperationId = BeginOperation();
	SetSessionState(EUSSessionState::Creating);

	CreateSessionCompleteHandle = Sessions->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateWeakLambda(this, [this, OperationId](FName /*SessionName*/, bool bWasSuccessful)
		{
			OnCreateSessionFinished(OperationId, bWasSuccessful);
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Creating session for %d players (LANMatch=%d)."), Settings.NumPublicConnections, bUseLANMatch ? 1 : 0);

	// Backends are allowed to fire the completion synchronously and still return
	// false, so only take the failure path if the completion has not run yet.
	if (!Sessions->CreateSession(SessionLocalUserNum, NAME_GameSession, Settings) && IsOperationPending(OperationId))
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("CreateSession was rejected by the backend before going async."));
		OnCreateSessionFinished(OperationId, false);
	}
}

void UUSSessionSubsystem::OnCreateSessionFinished(uint32 OperationId, bool bWasSuccessful)
{
	ClearCreateSessionDelegate();

	if (!ConsumeOperation(OperationId))
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Ignoring a CreateSession completion that is no longer current (Success=%d)."), bWasSuccessful ? 1 : 0);
		if (bWasSuccessful)
		{
			DestroyLeftoverSession(TEXT("an abandoned HostSession"));
		}
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;

	if (!bWasSuccessful || World == nullptr)
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("Session creation failed (Success=%d World=%s)."), bWasSuccessful ? 1 : 0, World ? TEXT("valid") : TEXT("null"));

		SetSessionState(EUSSessionState::Idle);
		DestroyLeftoverSession(TEXT("a failed HostSession"));
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	SetSessionState(EUSSessionState::Hosting);

	// Deliberately not calling StartSession: it moves the session to InProgress,
	// which some backends treat as no longer joinable. Co-op wants join-in-progress.
	const FString CurrentMap = UGameplayStatics::GetCurrentLevelName(World);
	UE_LOG(LogUnrealSplit, Log, TEXT("Session created. Reopening '%s' as a listen server."), *CurrentMap);
	UGameplayStatics::OpenLevel(World, FName(*CurrentMap), true, TEXT("listen"));

	OnSessionHostedDelegate.Broadcast(true);
}

void UUSSessionSubsystem::FindSessions()
{
	if (SessionState != EUSSessionState::Idle)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("FindSessions rejected: state is %s, expected Idle."), *UEnum::GetValueAsString(SessionState));
		OnSessionsFoundDelegate.Broadcast(0);
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance());
	if (!Sessions.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("FindSessions failed: no session interface. Check DefaultPlatformService in DefaultEngine.ini."));
		OnSessionsFoundDelegate.Broadcast(0);
		return;
	}

	FoundSessions.Reset();

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->bIsLanQuery = bUseLANMatch;
	SessionSearch->MaxSearchResults = SessionMaxSearchResults;
	SessionSearch->TimeoutInSeconds = SessionSearchTimeoutSeconds;

	const uint32 OperationId = BeginOperation();
	SetSessionState(EUSSessionState::Searching);

	FindSessionsCompleteHandle = Sessions->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateWeakLambda(this, [this, OperationId](bool bWasSuccessful)
		{
			OnFindSessionsFinished(OperationId, bWasSuccessful);
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Searching for sessions (LANQuery=%d, timeout %.0fs)."), bUseLANMatch ? 1 : 0, SessionSearchTimeoutSeconds);

	if (!Sessions->FindSessions(SessionLocalUserNum, SessionSearch.ToSharedRef()) && IsOperationPending(OperationId))
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("FindSessions was rejected by the backend before going async."));
		OnFindSessionsFinished(OperationId, false);
	}
}

void UUSSessionSubsystem::OnFindSessionsFinished(uint32 OperationId, bool bWasSuccessful)
{
	ClearFindSessionsDelegate();

	if (!ConsumeOperation(OperationId))
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Ignoring a FindSessions completion that is no longer current (Success=%d)."), bWasSuccessful ? 1 : 0);
		return;
	}

	FoundSessions.Reset();

	if (bWasSuccessful && SessionSearch.IsValid())
	{
		for (int32 Index = 0; Index < SessionSearch->SearchResults.Num(); ++Index)
		{
			const FOnlineSessionSearchResult& Result = SessionSearch->SearchResults[Index];

			// Session info is what a join actually needs; an owning user id is not
			// guaranteed on every backend.
			if (!Result.IsSessionInfoValid())
			{
				UE_LOG(LogUnrealSplit, Warning, TEXT("Skipping search result %d: session info is not joinable."), Index);
				continue;
			}

			FUSSessionSearchResultInfo& Info = FoundSessions.AddDefaulted_GetRef();
			Info.SearchResultIndex = Index;
			Info.OwnerName = Result.Session.OwningUserName;
			Info.PingInMs = Result.PingInMs;
			Info.OpenPublicConnections = Result.Session.NumOpenPublicConnections;
			Info.MaxPublicConnections = Result.Session.SessionSettings.NumPublicConnections;
		}
	}
	else
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("Session search failed or returned no usable search object."));
		SessionSearch.Reset();
	}

	SetSessionState(EUSSessionState::Idle);

	UE_LOG(LogUnrealSplit, Log, TEXT("Session search finished: %d joinable result(s)."), FoundSessions.Num());
	for (int32 Index = 0; Index < FoundSessions.Num(); ++Index)
	{
		const FUSSessionSearchResultInfo& Info = FoundSessions[Index];
		UE_LOG(LogUnrealSplit, Log, TEXT("  [%d] Owner='%s' Ping=%dms Slots=%d/%d"),
			Index, *Info.OwnerName, Info.PingInMs, Info.OpenPublicConnections, Info.MaxPublicConnections);
	}

	OnSessionsFoundDelegate.Broadcast(FoundSessions.Num());
}

void UUSSessionSubsystem::JoinFoundSession(int32 SearchResultIndex)
{
	if (SessionState != EUSSessionState::Idle)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("JoinFoundSession rejected: state is %s, expected Idle."), *UEnum::GetValueAsString(SessionState));
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	if (!FoundSessions.IsValidIndex(SearchResultIndex) || !SessionSearch.IsValid())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("JoinFoundSession rejected: index %d is not in the %d cached result(s). Run a search first."), SearchResultIndex, FoundSessions.Num());
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	const int32 BackendIndex = FoundSessions[SearchResultIndex].SearchResultIndex;
	if (!SessionSearch->SearchResults.IsValidIndex(BackendIndex))
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("JoinFoundSession rejected: cached result %d no longer maps to a backend result."), SearchResultIndex);
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance());
	if (!Sessions.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("JoinFoundSession failed: no session interface. Check DefaultPlatformService in DefaultEngine.ini."));
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	if (Sessions->GetNamedSession(NAME_GameSession) != nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("JoinFoundSession rejected: session '%s' already exists. Call LeaveSession first."), *FName(NAME_GameSession).ToString());
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	const uint32 OperationId = BeginOperation();
	SetSessionState(EUSSessionState::Joining);

	JoinSessionCompleteHandle = Sessions->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateWeakLambda(this, [this, OperationId](FName /*SessionName*/, EOnJoinSessionCompleteResult::Type Result)
		{
			OnJoinSessionFinished(OperationId, Result == EOnJoinSessionCompleteResult::Success, LexToString(Result));
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Joining session %d (owner '%s')."), SearchResultIndex, *FoundSessions[SearchResultIndex].OwnerName);

	if (!Sessions->JoinSession(SessionLocalUserNum, NAME_GameSession, SessionSearch->SearchResults[BackendIndex]) && IsOperationPending(OperationId))
	{
		OnJoinSessionFinished(OperationId, false, TEXT("rejected by the backend before going async"));
	}
}

void UUSSessionSubsystem::OnJoinSessionFinished(uint32 OperationId, bool bSuccess, const FString& ResultDescription)
{
	ClearJoinSessionDelegate();

	if (!ConsumeOperation(OperationId))
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Ignoring a JoinSession completion that is no longer current (Success=%d)."), bSuccess ? 1 : 0);
		if (bSuccess)
		{
			DestroyLeftoverSession(TEXT("an abandoned JoinFoundSession"));
		}
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	const IOnlineSessionPtr Sessions = GetSessionInterface(GameInstance);

	FString ConnectString;
	APlayerController* PlayerController = GameInstance ? GameInstance->GetFirstLocalPlayerController() : nullptr;

	if (!bSuccess)
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("Join failed: %s"), *ResultDescription);
	}
	else if (!Sessions.IsValid() || !Sessions->GetResolvedConnectString(NAME_GameSession, ConnectString) || ConnectString.IsEmpty())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("Join succeeded but the host address could not be resolved."));
		bSuccess = false;
	}
	else if (PlayerController == nullptr)
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("Join succeeded but there is no local player controller to travel with."));
		bSuccess = false;
	}

	if (!bSuccess)
	{
		SetSessionState(EUSSessionState::Idle);
		DestroyLeftoverSession(TEXT("a failed JoinFoundSession"));
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	SetSessionState(EUSSessionState::InSessionAsClient);

	UE_LOG(LogUnrealSplit, Log, TEXT("Joined session. Travelling to '%s'."), *ConnectString);
	PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);

	OnJoinCompletedDelegate.Broadcast(true);
}

void UUSSessionSubsystem::LeaveSession()
{
	if (SessionState == EUSSessionState::Leaving)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("LeaveSession rejected: teardown is already in flight."));
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance());
	const bool bHasSession = Sessions.IsValid() && Sessions->GetNamedSession(NAME_GameSession) != nullptr;

	if (SessionState == EUSSessionState::Idle && !bHasSession)
	{
		// Nothing to tear down, so don't yank the player out of the current map.
		UE_LOG(LogUnrealSplit, Warning, TEXT("LeaveSession ignored: no session is active."));
		return;
	}

	AbortSession(TEXT("LeaveSession"));
}

void UUSSessionSubsystem::AbortSession(const FString& Reason)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance());
	const bool bHasSession = Sessions.IsValid() && Sessions->GetNamedSession(NAME_GameSession) != nullptr;
	const bool bWasSearching = SessionState == EUSSessionState::Searching;

	// Claiming an operation abandons whatever was in flight. Create and join
	// completions stay bound on purpose: if one still lands, its handler sees a
	// stale id and destroys what the backend registered behind our back.
	const uint32 OperationId = BeginOperation();

	if (bWasSearching && Sessions.IsValid())
	{
		// A search must be cancelled rather than forgotten: backends silently
		// ignore a new search while an old one is still registered.
		ClearFindSessionsDelegate();
		Sessions->CancelFindSessions();
	}

	SessionSearch.Reset();
	FoundSessions.Reset();
	PendingSessionEndReason = Reason;

	if (!bHasSession)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Ending session (%s): nothing registered with the backend."), *Reason);
		OnDestroySessionFinished(OperationId, true);
		return;
	}

	SetSessionState(EUSSessionState::Leaving);

	DestroySessionCompleteHandle = Sessions->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateWeakLambda(this, [this, OperationId](FName /*SessionName*/, bool bWasSuccessful)
		{
			OnDestroySessionFinished(OperationId, bWasSuccessful);
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Destroying session (%s)."), *Reason);

	if (!Sessions->DestroySession(NAME_GameSession) && IsOperationPending(OperationId))
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("DestroySession was rejected by the backend before going async."));
		OnDestroySessionFinished(OperationId, false);
	}
}

void UUSSessionSubsystem::OnDestroySessionFinished(uint32 OperationId, bool bWasSuccessful)
{
	ClearDestroySessionDelegate();

	if (!ConsumeOperation(OperationId))
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Ignoring a DestroySession completion that is no longer current (Success=%d)."), bWasSuccessful ? 1 : 0);
		return;
	}

	if (!bWasSuccessful)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("Session teardown reported failure; continuing to the entry map anyway."));
	}

	const FString Reason = PendingSessionEndReason.IsEmpty() ? FString(TEXT("Unknown")) : PendingSessionEndReason;
	PendingSessionEndReason.Reset();

	SetSessionState(EUSSessionState::Idle);
	ReturnToEntryMap();

	UE_LOG(LogUnrealSplit, Log, TEXT("Session ended: %s"), *Reason);
	OnSessionEndedDelegate.Broadcast(Reason);
}

void UUSSessionSubsystem::HandleNetworkFailure(UWorld* World, UNetDriver* /*NetDriver*/, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	// GEngine broadcasts globally; PIE runs several game instances at once.
	if (World == nullptr || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Error, TEXT("Network failure: %s ('%s')."), ENetworkFailure::ToString(FailureType), *ErrorString);

	if (SessionState == EUSSessionState::Idle || SessionState == EUSSessionState::Leaving)
	{
		return;
	}

	AbortSession(FString::Printf(TEXT("NetworkFailure:%s"), ENetworkFailure::ToString(FailureType)));
}

void UUSSessionSubsystem::HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString)
{
	if (World == nullptr || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Error, TEXT("Travel failure: %s ('%s')."), ETravelFailure::ToString(FailureType), *ErrorString);

	if (SessionState == EUSSessionState::Idle || SessionState == EUSSessionState::Leaving)
	{
		return;
	}

	AbortSession(FString::Printf(TEXT("TravelFailure:%s"), ETravelFailure::ToString(FailureType)));
}

uint32 UUSSessionSubsystem::BeginOperation()
{
	return ++PendingOperationId;
}

bool UUSSessionSubsystem::IsOperationPending(uint32 OperationId) const
{
	return OperationId == PendingOperationId;
}

bool UUSSessionSubsystem::ConsumeOperation(uint32 OperationId)
{
	if (OperationId != PendingOperationId)
	{
		return false;
	}

	++PendingOperationId;
	return true;
}

void UUSSessionSubsystem::DestroyLeftoverSession(const FString& Context)
{
	if (SessionState != EUSSessionState::Idle)
	{
		// Another operation already owns the session name and will clean up itself;
		// destroying here would pull the session out from under it.
		UE_LOG(LogUnrealSplit, Log, TEXT("Leaving the session from %s alone: state is %s."), *Context, *UEnum::GetValueAsString(SessionState));
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance());
	if (!Sessions.IsValid() || Sessions->GetNamedSession(NAME_GameSession) == nullptr)
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Warning, TEXT("Destroying the session left behind by %s."), *Context);

	// Uses the per-call completion delegate so the outcome is reported without
	// touching the shared handle the state machine relies on.
	Sessions->DestroySession(NAME_GameSession,
		FOnDestroySessionCompleteDelegate::CreateWeakLambda(this, [Context](FName /*SessionName*/, bool bWasSuccessful)
		{
			if (bWasSuccessful)
			{
				UE_LOG(LogUnrealSplit, Log, TEXT("Session left behind by %s destroyed."), *Context);
			}
			else
			{
				UE_LOG(LogUnrealSplit, Error, TEXT("Failed to destroy the session left behind by %s; the backend may still hold it."), *Context);
			}
		}));
}

void UUSSessionSubsystem::ReturnToEntryMap()
{
	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (World == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("Cannot return to the entry map: no world."));
		return;
	}

	FString EntryMap = EntryMapOverride.IsEmpty() ? UGameMapsSettings::GetGameDefaultMap() : EntryMapOverride;
	EntryMap = FPackageName::ObjectPathToPackageName(EntryMap);
	if (EntryMap.IsEmpty())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("No entry map configured; staying on the current map."));
		return;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Returning to entry map '%s'."), *EntryMap);
	UGameplayStatics::OpenLevel(World, FName(*EntryMap), true);
}

void UUSSessionSubsystem::ClearCreateSessionDelegate()
{
	if (!CreateSessionCompleteHandle.IsValid())
	{
		return;
	}

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance()))
	{
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteHandle);
	}
	CreateSessionCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearFindSessionsDelegate()
{
	if (!FindSessionsCompleteHandle.IsValid())
	{
		return;
	}

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance()))
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteHandle);
	}
	FindSessionsCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearJoinSessionDelegate()
{
	if (!JoinSessionCompleteHandle.IsValid())
	{
		return;
	}

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance()))
	{
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteHandle);
	}
	JoinSessionCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearDestroySessionDelegate()
{
	if (!DestroySessionCompleteHandle.IsValid())
	{
		return;
	}

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(GetGameInstance()))
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteHandle);
	}
	DestroySessionCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearSessionDelegates()
{
	ClearCreateSessionDelegate();
	ClearFindSessionsDelegate();
	ClearJoinSessionDelegate();
	ClearDestroySessionDelegate();
}

void UUSSessionSubsystem::SetSessionState(EUSSessionState NewState)
{
	if (SessionState == NewState)
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Session state: %s -> %s"),
		*UEnum::GetValueAsString(SessionState), *UEnum::GetValueAsString(NewState));

	SessionState = NewState;
}
