// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USSessionSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameMapsSettings.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "UObject/Class.h"

#include "USConsoleReport.h"
#include "UnrealSplitModule.h"
#include "UnrealSplitStartupModule.h"

namespace
{
	/** Local player index driving session calls; two-player co-op only ever hosts from player 0. */
	constexpr int32 SessionLocalUserNum = 0;

	/** A LAN co-op lobby list never needs to be long. */
	constexpr int32 SessionMaxSearchResults = 20;

	/** Backends that honour a search timeout give up after this long. */
	constexpr float SessionSearchTimeoutSeconds = 15.0f;

	/**
	 * Key under which a hosted service files this game's sessions, and the only
	 * one a search of that service ever looks in.
	 *
	 * The name is EOS's, spelled out rather than included: it comes from
	 * OSSEOS_BUCKET_ID_ATTRIBUTE_KEY in the EOS plugin's
	 * OnlineSubsystemEOSTypesPublic.h, which is compiled out without the EOS SDK
	 * and would tie this module to one backend for the sake of a string. Backends
	 * that have no such concept ignore an unknown setting.
	 */
	const FName SessionBucketIdKey(TEXT("OSSEOS_BUCKET_ID_ATTRIBUTE_KEY"));

	/**
	 * Name half of the bucket. The build half comes from GetBuildUniqueId, so two
	 * incompatible builds cannot discover each other — the same separation the
	 * backend's own default gives, kept deliberately rather than by accident.
	 */
	const TCHAR* SessionBucketPrefix = TEXT("UnrealSplit");

	/**
	 * Session interface of the process-default online subsystem: the one backend
	 * this process runs on, and the same one every engine-internal online call
	 * reaches for. Online::* does the PIE instance-name juggling, which is why the
	 * world is required.
	 */
	IOnlineSessionPtr GetSessionInterface(const UUSSessionSubsystem* Subsystem)
	{
		const UGameInstance* GameInstance = Subsystem ? Subsystem->GetGameInstance() : nullptr;
		const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
		return World ? Online::GetSessionInterface(World) : nullptr;
	}

	/** Identity interface of the same backend; missing on backends without one. */
	IOnlineIdentityPtr GetIdentityInterface(const UUSSessionSubsystem* Subsystem)
	{
		const UGameInstance* GameInstance = Subsystem ? Subsystem->GetGameInstance() : nullptr;
		const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
		return World ? Online::GetIdentityInterface(World) : nullptr;
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

	// Global rather than per-game-mode, so registration works no matter which
	// game mode class the game uses.
	PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(this, &UUSSessionSubsystem::HandleGameModePostLogin);

	// Read, not chosen: UnrealSplitStartup already put the backend in place before
	// the engine read it, and it cannot change for the life of the process. No
	// login here either — there is no world yet, so resolving the backend would
	// reach past PIE's per-instance subsystems; that happens on the first session
	// call that needs it.
	ConnectionMode = UnrealSplitStartup::IsLanBackend() ? EUSConnectionMode::LAN : EUSConnectionMode::Internet;

	UE_LOG(LogUnrealSplit, Log, TEXT("Session subsystem initialized. Mode=%s Backend=%s MaxTotalPlayers=%d"),
		*UEnum::GetValueAsString(ConnectionMode), *UnrealSplitStartup::GetBackendName().ToString(), MaxTotalPlayers);
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

	FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
	PostLoginHandle.Reset();

	PendingLoginLocalUserIndex = INDEX_NONE;
	PendingLoginPlatformUser = PLATFORMUSERID_NONE;
	bBackendLoginInFlight = false;

	SessionSearch.Reset();
	FoundSessions.Reset();

	UE_LOG(LogUnrealSplit, Log, TEXT("Session subsystem deinitialized."));

	Super::Deinitialize();
}

FName UUSSessionSubsystem::GetBackendName() const
{
	// The live subsystem where there is one, so a backend that failed to come up
	// and fell back to Null reports what it actually is rather than what was asked
	// for. Before a world exists only the startup answer is available.
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	const IOnlineSubsystem* Backend = World ? Online::GetSubsystem(World) : nullptr;

	return Backend != nullptr ? Backend->GetSubsystemName() : UnrealSplitStartup::GetBackendName();
}

bool UUSSessionSubsystem::IsLanMatch() const
{
	return ConnectionMode == EUSConnectionMode::LAN;
}

bool UUSSessionSubsystem::DoesModeUseLobbies() const
{
	// A lobby is how a hosted service lets a client join a host that has no
	// routable address of its own, so it belongs to internet play only. LAN
	// resolves the host directly from the broadcast reply.
	return !IsLanMatch() && bInternetUsesLobbies;
}

FString UUSSessionSubsystem::GetSessionBucketId() const
{
	return FString::Printf(TEXT("%s:%d"), SessionBucketPrefix, GetBuildUniqueId());
}

void UUSSessionSubsystem::ApplyDiscoveryKeys(FOnlineSessionSettings& Settings) const
{
	Settings.bIsLANMatch = IsLanMatch();
	Settings.bUseLobbiesIfAvailable = DoesModeUseLobbies();

	if (IsLanMatch())
	{
		// LAN discovery is a broadcast on the local wire, with no registry to file
		// the session in and no query to match it against.
		return;
	}

	// ViaOnlineService rather than DontAdvertise, and that is load bearing. The
	// backend reads this setting to pick the bucket, but it also turns the
	// search's copy of it into an ordinary equality filter over advertised
	// attributes, so a bucket the host keeps to itself matches nothing.
	Settings.Set(SessionBucketIdKey, GetSessionBucketId(), EOnlineDataAdvertisementType::ViaOnlineService);
}

void UUSSessionSubsystem::ApplyDiscoveryKeys(FOnlineSessionSearch& Search) const
{
	Search.bIsLanQuery = IsLanMatch();

	if (IsLanMatch())
	{
		return;
	}

	// Without this the backend searches sessions proper, whatever the host
	// registered. A host that advertised a lobby is then simply not in the
	// registry being searched, and the search succeeds with nothing in it.
	Search.QuerySettings.Set(SEARCH_LOBBIES, DoesModeUseLobbies(), EOnlineComparisonOp::Equals);
	Search.QuerySettings.Set(SessionBucketIdKey, GetSessionBucketId(), EOnlineComparisonOp::Equals);
}

FString UUSSessionSubsystem::DescribeDiscoveryKeys() const
{
	if (IsLanMatch())
	{
		return TEXT("Kind=LAN broadcast");
	}

	return FString::Printf(TEXT("Kind=%s Bucket='%s'"),
		DoesModeUseLobbies() ? TEXT("lobby") : TEXT("session"), *GetSessionBucketId());
}

bool UUSSessionSubsystem::DoesModeRequireLogin() const
{
	return ConnectionMode == EUSConnectionMode::Internet ? bInternetRequiresLogin : bLanRequiresLogin;
}

bool UUSSessionSubsystem::IsBackendInstalled() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;

	// Before a world exists only the config answer is available, and it is the
	// one that matters: whether the module is enabled in this build.
	return World != nullptr
		? Online::GetSubsystem(World) != nullptr
		: IOnlineSubsystem::IsEnabled(UnrealSplitStartup::GetBackendName());
}

bool UUSSessionSubsystem::IsBackendReady() const
{
	if (!DoesModeRequireLogin())
	{
		return true;
	}

	const IOnlineIdentityPtr Identity = GetIdentityInterface(this);
	if (!Identity.IsValid())
	{
		// A backend without identities cannot be logged into, so waiting for one
		// would hang the mode forever.
		return true;
	}

	return Identity->GetLoginStatus(SessionLocalUserNum) == ELoginStatus::LoggedIn;
}

EUSBackendStatus UUSSessionSubsystem::GetBackendStatus() const
{
	if (!IsBackendInstalled())
	{
		return EUSBackendStatus::NotInstalled;
	}

	if (IsBackendReady())
	{
		return EUSBackendStatus::Ready;
	}

	return bBackendLoginInFlight ? EUSBackendStatus::LoggingIn : EUSBackendStatus::NotLoggedIn;
}

bool UUSSessionSubsystem::LoginToBackend()
{
	if (IsBackendReady())
	{
		OnBackendReadyDelegate.Broadcast(true);
		return true;
	}

	if (bBackendLoginInFlight)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Backend login already in flight; waiting for it."));
		return true;
	}

	const IOnlineIdentityPtr Identity = GetIdentityInterface(this);
	if (!Identity.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("LoginToBackend failed: backend '%s' has no identity interface, but mode %s is configured to require a login."),
			*GetBackendName().ToString(), *UEnum::GetValueAsString(ConnectionMode));
		OnBackendReadyDelegate.Broadcast(false);
		return false;
	}

	bBackendLoginInFlight = true;

	BackendLoginCompleteHandle = Identity->AddOnLoginCompleteDelegate_Handle(SessionLocalUserNum,
		FOnLoginCompleteDelegate::CreateWeakLambda(this, [this](int32 InLocalUserIndex, bool bWasSuccessful, const FUniqueNetId& /*UserId*/, const FString& ErrorString)
		{
			OnBackendLoginFinished(InLocalUserIndex, bWasSuccessful, ErrorString);
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Logging the primary user into backend '%s'."), *GetBackendName().ToString());

	// Same contract as everywhere else here: a backend may run the completion
	// synchronously and still report failure from the call itself.
	if (!Identity->AutoLogin(SessionLocalUserNum) && bBackendLoginInFlight)
	{
		OnBackendLoginFinished(SessionLocalUserNum, false, TEXT("AutoLogin was rejected by the backend before going async"));
		return false;
	}

	return true;
}

void UUSSessionSubsystem::OnBackendLoginFinished(int32 LocalUserIndex, bool bWasSuccessful, const FString& ErrorString)
{
	if (LocalUserIndex != SessionLocalUserNum || !bBackendLoginInFlight)
	{
		return;
	}

	ClearBackendLoginDelegate();
	bBackendLoginInFlight = false;

	if (bWasSuccessful)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Backend '%s' is ready: the primary user is logged in."), *GetBackendName().ToString());
	}
	else
	{
		// Named explicitly because the usual cause is credentials or a missing
		// developer auth tool, not anything the session layer can retry itself.
		// On EOS specifically, an application that has not passed brand review
		// only authenticates Epic accounts belonging to the developer's
		// organisation, which fails here and nowhere else.
		UE_LOG(LogUnrealSplit, Error, TEXT("Login to backend '%s' failed ('%s'). Hosting and searching stay refused in mode %s until a login succeeds. Check this process's credentials: on EOS, the account the Developer Authentication Tool served, and whether that account is a member of the organisation that owns the application."),
			*GetBackendName().ToString(), *ErrorString, *UEnum::GetValueAsString(ConnectionMode));
	}

	OnBackendReadyDelegate.Broadcast(bWasSuccessful);
}

bool UUSSessionSubsystem::EnsureBackendReady(const TCHAR* Context)
{
	if (!IsBackendInstalled())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("%s failed: no online subsystem came up for mode %s (expected '%s'). Check that its plugin is enabled and DefaultPlatformService names it."),
			Context, *UEnum::GetValueAsString(ConnectionMode), *GetBackendName().ToString());
		return false;
	}

	if (IsBackendReady())
	{
		return true;
	}

	// Error rather than Warning, and spelled out rather than left to the caller's
	// "0 results": every entry point here reports a refusal the same way it
	// reports an empty answer, so without a line that says which of the two this
	// was, a process that is simply not signed in looks exactly like a process
	// that searched a working backend and found nothing there.
	const IOnlineIdentityPtr Identity = GetIdentityInterface(this);
	const ELoginStatus::Type LoginStatus = Identity.IsValid() ? Identity->GetLoginStatus(SessionLocalUserNum) : ELoginStatus::NotLoggedIn;

	UE_LOG(LogUnrealSplit, Error, TEXT("%s refused: NOT SIGNED IN. Backend '%s' reports login status %s for local user %d, and mode %s requires one. This is a sign-in failure, not an empty result. Starting the login; retry when OnBackendReadyDelegate reports success."),
		Context, *GetBackendName().ToString(), ELoginStatus::ToString(LoginStatus), SessionLocalUserNum, *UEnum::GetValueAsString(ConnectionMode));

	LoginToBackend();
	return false;
}

void UUSSessionSubsystem::HostSession(int32 MaxPlayers)
{
	if (SessionState != EUSSessionState::Idle)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("HostSession rejected: state is %s, expected Idle."), *UEnum::GetValueAsString(SessionState));
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	if (!EnsureBackendReady(TEXT("HostSession")))
	{
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
	if (!Sessions.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("HostSession failed: backend '%s' has no session interface."), *GetBackendName().ToString());
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	if (Sessions->GetNamedSession(NAME_GameSession) != nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("HostSession rejected: session '%s' already exists. Call LeaveSession first."), *FName(NAME_GameSession).ToString());
		OnSessionHostedDelegate.Broadcast(false);
		return;
	}

	// Advertising more seats than the game will fill only produces clients that
	// travel in and get refused at login, so the cap wins over the argument.
	const int32 AdvertisedPlayers = FMath::Clamp(MaxPlayers, 1, FMath::Max(1, MaxTotalPlayers));
	if (AdvertisedPlayers != MaxPlayers)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("HostSession asked for %d players; advertising %d, the MaxTotalPlayers cap."), MaxPlayers, AdvertisedPlayers);
	}

	FOnlineSessionSettings Settings;
	// Public rather than private, and never zero: a hosted service reads the
	// public count as "may anyone find this", and a session with none is
	// registered invisibly no matter what bShouldAdvertise says.
	Settings.NumPublicConnections = AdvertisedPlayers;
	Settings.NumPrivateConnections = 0;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	Settings.bIsDedicated = false;
	// Presence is a friends-and-invites feature; sessions here are always
	// reached through a search, and asking for presence would need scopes the
	// game deliberately does not request.
	Settings.bUsesPresence = false;
	Settings.bAllowJoinViaPresence = false;
	Settings.bAllowJoinViaPresenceFriendsOnly = false;
	Settings.bAllowInvites = false;
	ApplyDiscoveryKeys(Settings);

	const uint32 OperationId = BeginOperation();
	SetSessionState(EUSSessionState::Creating);

	CreateSessionCompleteHandle = Sessions->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateWeakLambda(this, [this, OperationId](FName /*SessionName*/, bool bWasSuccessful)
		{
			OnCreateSessionFinished(OperationId, bWasSuccessful);
		}));

	// Everything a search has to agree with, on one line: whoever compares this
	// against the searching process's line finds the mismatch by reading, not by
	// guessing which of the two sides is wrong.
	UE_LOG(LogUnrealSplit, Log, TEXT("Creating session for %d players on backend '%s'. %s Advertise=%d LANMatch=%d JoinInProgress=%d Presence=%d"),
		Settings.NumPublicConnections, *GetBackendName().ToString(), *DescribeDiscoveryKeys(),
		Settings.bShouldAdvertise ? 1 : 0, Settings.bIsLANMatch ? 1 : 0,
		Settings.bAllowJoinInProgress ? 1 : 0, Settings.bUsesPresence ? 1 : 0);

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

	// The listen reopen below re-runs every local login, and the PostLogin hook
	// registers players then too. Registering here as well covers flows that
	// never travel, at no cost: registration is idempotent.
	RegisterLocalPlayersWithSession(TEXT("HostSession"));

	// Deliberately not calling StartSession: it moves the session from Pending to
	// InProgress, which backends treat as no longer joinable unless join-in-
	// progress is allowed. Co-op wants join-in-progress, and staying Pending
	// keeps the session findable without depending on that flag being honoured.
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

	if (!EnsureBackendReady(TEXT("FindSessions")))
	{
		OnSessionsFoundDelegate.Broadcast(0);
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
	if (!Sessions.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("FindSessions failed: backend '%s' has no session interface."), *GetBackendName().ToString());
		OnSessionsFoundDelegate.Broadcast(0);
		return;
	}

	FoundSessions.Reset();

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = SessionMaxSearchResults;
	SessionSearch->TimeoutInSeconds = SessionSearchTimeoutSeconds;
	ApplyDiscoveryKeys(*SessionSearch);

	// Deliberately no filters past the discovery keys. Free seats especially:
	// with a two-seat cap, a host that has taken its own seat has one left, and
	// a minimum-slots filter set from this machine's player count would drop
	// exactly the session it is looking for. Capacity is checked in
	// JoinFoundSession instead, where the real numbers are known.

	const uint32 OperationId = BeginOperation();
	SetSessionState(EUSSessionState::Searching);

	FindSessionsCompleteHandle = Sessions->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateWeakLambda(this, [this, OperationId](bool bWasSuccessful)
		{
			OnFindSessionsFinished(OperationId, bWasSuccessful);
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Searching for sessions on backend '%s'. %s LANQuery=%d MaxResults=%d Timeout=%.0fs"),
		*GetBackendName().ToString(), *DescribeDiscoveryKeys(),
		SessionSearch->bIsLanQuery ? 1 : 0, SessionMaxSearchResults, SessionSearchTimeoutSeconds);

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

	int32 RawResults = 0;

	if (bWasSuccessful && SessionSearch.IsValid())
	{
		RawResults = SessionSearch->SearchResults.Num();

		for (int32 Index = 0; Index < RawResults; ++Index)
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
			Info.SessionId = Result.GetSessionIdStr();
			Info.OwnerName = Result.Session.OwningUserName;
			Info.PingInMs = Result.PingInMs;
			Info.OpenPublicConnections = Result.Session.NumOpenPublicConnections;
			Info.MaxPublicConnections = Result.Session.SessionSettings.NumPublicConnections;
		}
	}
	else
	{
		USConsole::ReportAsyncWarning(GetGameInstance(), TEXT("Session search failed or returned no usable search object."));
		SessionSearch.Reset();
	}

	SetSessionState(EUSSessionState::Idle);

	// A search is started from the console and resolves seconds later, so its
	// result goes back to the console that asked for it as well as to the log.
	// Raw alongside joinable, because "the backend returned nothing" and "the
	// backend returned sessions this build cannot join" are different problems
	// that a single count hides.
	USConsole::ReportAsync(GetGameInstance(), FString::Printf(TEXT("Session search finished on backend '%s'. %s Raw=%d Joinable=%d"),
		*GetBackendName().ToString(), *DescribeDiscoveryKeys(), RawResults, FoundSessions.Num()));

	for (int32 Index = 0; Index < FoundSessions.Num(); ++Index)
	{
		const FUSSessionSearchResultInfo& Info = FoundSessions[Index];
		USConsole::ReportAsync(GetGameInstance(), FString::Printf(TEXT("  [%d] Id='%s' Owner='%s' Ping=%dms Slots=%d/%d — join with US.Join %d"),
			Index, *Info.SessionId, *Info.OwnerName, Info.PingInMs, Info.OpenPublicConnections, Info.MaxPublicConnections, Index));
	}

	if (bWasSuccessful && RawResults == 0)
	{
		// The one outcome with no evidence in it at all. Naming what is left to
		// check is the difference between reading the next log line and starting
		// a debugging session.
		USConsole::ReportAsyncWarning(GetGameInstance(), FString::Printf(TEXT("The backend accepted the search and returned nothing. The local user IS signed in, so this is not a login problem. Compare this line's %s against the host's 'Creating session' line: a host that advertised a different kind or a different bucket is invisible here. Then check that the host is still running and did advertise."),
			*DescribeDiscoveryKeys()));
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

	// Every local player travels along and asks the host for a seat, so a machine
	// with more players than the host has free seats cannot join at all. Checked
	// here because the host would otherwise accept the first player and refuse the
	// rest mid-travel, which drops the whole connection.
	// Counts local players, not game players: if the shadow view of plan step 1.6
	// ends up being a controller-less ULocalPlayer, it has to be excluded here.
	const int32 NumLocalPlayers = GetGameInstance()->GetNumLocalPlayers();
	const int32 OpenSeats = FoundSessions[SearchResultIndex].OpenPublicConnections;
	if (NumLocalPlayers > OpenSeats)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("JoinFoundSession rejected: this machine has %d local player(s) but the session only has %d free seat(s). Remove a local player or pick another session."),
			NumLocalPlayers, OpenSeats);
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	if (!EnsureBackendReady(TEXT("JoinFoundSession")))
	{
		OnJoinCompletedDelegate.Broadcast(false);
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
	if (!Sessions.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("JoinFoundSession failed: backend '%s' has no session interface."), *GetBackendName().ToString());
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

	UE_LOG(LogUnrealSplit, Log, TEXT("Joining session %d (Id='%s' owner '%s')."),
		SearchResultIndex, *FoundSessions[SearchResultIndex].SessionId, *FoundSessions[SearchResultIndex].OwnerName);

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
	const IOnlineSessionPtr Sessions = GetSessionInterface(this);

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

	// The client keeps its own copy of the session; its local players belong in
	// it just like on the host. The host registers them on its side when their
	// logins arrive through the PostLogin hook.
	RegisterLocalPlayersWithSession(TEXT("JoinFoundSession"));

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

	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
	const bool bHasSession = Sessions.IsValid() && Sessions->GetNamedSession(NAME_GameSession) != nullptr;

	if (SessionState == EUSSessionState::Idle && !bHasSession)
	{
		// Nothing to tear down, so don't yank the player out of the current map.
		UE_LOG(LogUnrealSplit, Warning, TEXT("LeaveSession ignored: no session is active."));
		return;
	}

	AbortSession(TEXT("LeaveSession"));
}

bool UUSSessionSubsystem::AddLocalCoopPlayer()
{
	UGameInstance* GameInstance = GetGameInstance();
	const int32 NumLocalPlayers = GameInstance->GetNumLocalPlayers();

	// Counts remote players too: a client whose host already fills the game gets
	// no second seat here, and the engine would refuse the split join anyway.
	const int32 KnownPlayers = GetKnownPlayerCount();
	if (KnownPlayers >= MaxTotalPlayers)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("AddLocalCoopPlayer rejected: the game already has %d of %d player(s) (%d local on this machine)."), KnownPlayers, MaxTotalPlayers, NumLocalPlayers);
		return false;
	}

	if (PendingLoginLocalUserIndex != INDEX_NONE)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("AddLocalCoopPlayer rejected: the online login of local user %d is still in flight."), PendingLoginLocalUserIndex);
		return false;
	}

	if (SessionState == EUSSessionState::Creating || SessionState == EUSSessionState::Joining || SessionState == EUSSessionState::Leaving)
	{
		// The player would log in against a world that is about to be replaced by
		// travel, and register into a session that may not survive the operation.
		UE_LOG(LogUnrealSplit, Warning, TEXT("AddLocalCoopPlayer rejected: state is %s. Wait for the session operation to resolve."), *UEnum::GetValueAsString(SessionState));
		return false;
	}

	// Ask the platform for the user at the next seat instead of assuming a
	// controller id. The platform's device mapper owns which physical devices
	// drive that user: on desktop, keyboard/mouse and gamepad 0 stay with the
	// primary user, and each later user gets the gamepad matching its index.
	const FPlatformUserId UserId = IPlatformInputDeviceMapper::Get().GetPlatformUserForUserIndex(NumLocalPlayers);
	if (!UserId.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("AddLocalCoopPlayer failed: the platform did not provide a user for local player index %d."), NumLocalPlayers);
		return false;
	}

	// The player's session identity is taken from its local player at login
	// time and never revisited: the authority reads it in InitNewPlayer, and a
	// client puts it straight into the split-join message. Backends only
	// auto-login the primary user, so without logging this one in first the
	// player would carry an invalid id and could never be registered.
	const IOnlineIdentityPtr Identity = GetIdentityInterface(this);
	const int32 LocalUserIndex = IPlatformInputDeviceMapper::Get().GetUserIndexForPlatformUser(UserId);
	const bool bNeedsLogin = Identity.IsValid() && LocalUserIndex >= 0 && Identity->GetLoginStatus(LocalUserIndex) != ELoginStatus::LoggedIn;

	if (!bNeedsLogin)
	{
		// Either already logged in, or a backend without identities at all —
		// pure couch co-op works without one.
		return CreateLocalCoopPlayerForUser(UserId);
	}

	PendingLoginLocalUserIndex = LocalUserIndex;
	PendingLoginPlatformUser = UserId;

	LoginCompleteHandle = Identity->AddOnLoginCompleteDelegate_Handle(LocalUserIndex,
		FOnLoginCompleteDelegate::CreateWeakLambda(this, [this](int32 InLocalUserIndex, bool bWasSuccessful, const FUniqueNetId& /*UserId*/, const FString& ErrorString)
		{
			OnLocalCoopPlayerLoginFinished(InLocalUserIndex, bWasSuccessful, ErrorString);
		}));

	UE_LOG(LogUnrealSplit, Log, TEXT("Logging local user %d into the online backend before adding it as a local player."), LocalUserIndex);

	// Same contract as the session calls: a backend may run the completion
	// synchronously and still report failure from the call itself.
	if (!Identity->AutoLogin(LocalUserIndex) && PendingLoginLocalUserIndex == LocalUserIndex)
	{
		OnLocalCoopPlayerLoginFinished(LocalUserIndex, false, TEXT("AutoLogin was rejected by the backend before going async"));
		return false;
	}

	if (PendingLoginLocalUserIndex != INDEX_NONE)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Waiting for the login of local user %d; the player is added once it completes."), LocalUserIndex);
		return false;
	}

	// Synchronous backends (Null/LAN) already ran the completion above.
	return GameInstance->GetNumLocalPlayers() > NumLocalPlayers;
}

bool UUSSessionSubsystem::CreateLocalCoopPlayerForUser(FPlatformUserId UserId)
{
	UGameInstance* GameInstance = GetGameInstance();
	const int32 NumLocalPlayers = GameInstance->GetNumLocalPlayers();

	// Re-checked because an asynchronous login resolves long after the request,
	// and a remote player may have joined in the meantime.
	const int32 KnownPlayers = GetKnownPlayerCount();
	if (KnownPlayers >= MaxTotalPlayers)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("AddLocalCoopPlayer rejected: the game already has %d of %d player(s) (%d local on this machine)."), KnownPlayers, MaxTotalPlayers, NumLocalPlayers);
		return false;
	}

	FString Error;
	const ULocalPlayer* NewPlayer = GameInstance->CreateLocalPlayer(UserId, Error, /*bSpawnPlayerController*/ true);
	if (NewPlayer == nullptr)
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("AddLocalCoopPlayer failed: %s"), *Error);
		return false;
	}

	TArray<FInputDeviceId> PairedDevices;
	IPlatformInputDeviceMapper::Get().GetAllInputDevicesForUser(UserId, PairedDevices);
	UE_LOG(LogUnrealSplit, Log, TEXT("Local co-op player added: now %d local player(s), PlatformUser=%d with %d paired input device(s)."),
		GameInstance->GetNumLocalPlayers(), UserId.GetInternalId(), PairedDevices.Num());

	// A backend that derives ids from the machine alone gives every local user the
	// same one, and the session then counts two couch players as one. Config fixes
	// it per backend (Null: bAddUserNumToNullId), so say so instead of miscounting
	// quietly.
	const FUniqueNetIdRepl NewPlayerId = NewPlayer->GetPreferredUniqueNetId();
	if (NewPlayerId.IsValid())
	{
		for (const ULocalPlayer* ExistingPlayer : GameInstance->GetLocalPlayers())
		{
			if (ExistingPlayer != nullptr && ExistingPlayer != NewPlayer && ExistingPlayer->GetPreferredUniqueNetId() == NewPlayerId)
			{
				UE_LOG(LogUnrealSplit, Warning, TEXT("Local players share the online id '%s', so the session counts them as one player. Give each local user its own identity in the active backend's config."), *NewPlayerId.ToDebugString());
				break;
			}
		}
	}

	// On the authority the login this triggered already ran through the PostLogin
	// hook, which registered the player in any live session. A client instead has
	// to ask the host, which CreateLocalPlayer did for us.
	const UWorld* World = GameInstance->GetWorld();
	if (World && World->GetNetMode() == NM_Client)
	{
		// The split join carries this player's unique net id
		// (ULocalPlayer::SendSplitJoin), so the pawn appears when the host answers
		// and the host's PostLogin registers it there. Nothing to register locally
		// — the client's session copy is the engine's to update when the new
		// player state replicates in.
		UE_LOG(LogUnrealSplit, Log, TEXT("Asked the host for a split join for this player. A host without a free public connection disconnects the whole client, so host with MaxPlayers high enough for every couch player."));
	}

	return true;
}

int32 UUSSessionSubsystem::GetKnownPlayerCount() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return 0;
	}

	// GetNumPlayers is not const, and the authority is the only place with the
	// full picture: its own local players, split-join children, remote clients.
	if (AGameModeBase* GameMode = World->GetAuthGameMode())
	{
		return GameMode->GetNumPlayers();
	}

	if (const AGameStateBase* GameState = World->GetGameState())
	{
		// A client can only know the replicated list, which is what it should
		// judge by anyway — the host makes the final call at login.
		return GameState->PlayerArray.Num();
	}

	return GameInstance->GetNumLocalPlayers();
}

void UUSSessionSubsystem::OnLocalCoopPlayerLoginFinished(int32 LocalUserIndex, bool bWasSuccessful, const FString& ErrorString)
{
	if (LocalUserIndex != PendingLoginLocalUserIndex)
	{
		// A login this subsystem did not ask for, or one it already handled.
		return;
	}

	// Clears against the pending index, so it has to run before that is reset.
	ClearLoginDelegate();

	const FPlatformUserId UserId = PendingLoginPlatformUser;
	PendingLoginLocalUserIndex = INDEX_NONE;
	PendingLoginPlatformUser = PLATFORMUSERID_NONE;

	if (!bWasSuccessful)
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("AddLocalCoopPlayer failed: could not log local user %d into the online backend ('%s')."), LocalUserIndex, *ErrorString);
		return;
	}

	CreateLocalCoopPlayerForUser(UserId);
}

bool UUSSessionSubsystem::RemoveLocalCoopPlayer()
{
	UGameInstance* GameInstance = GetGameInstance();
	const int32 NumLocalPlayers = GameInstance->GetNumLocalPlayers();

	if (NumLocalPlayers <= 1)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("RemoveLocalCoopPlayer rejected: the primary local player cannot be removed."));
		return false;
	}

	const UWorld* World = GameInstance->GetWorld();
	if (World && World->GetNetMode() == NM_Client)
	{
		// UGameInstance::RemoveLocalPlayer would drop the viewport but never
		// tell the server, stranding the player's pawn in the session.
		UE_LOG(LogUnrealSplit, Warning, TEXT("RemoveLocalCoopPlayer rejected while connected to a remote host. Leave the session first."));
		return false;
	}

	ULocalPlayer* PlayerToRemove = GameInstance->GetLocalPlayerByIndex(NumLocalPlayers - 1);
	const FPlatformUserId UserId = PlayerToRemove->GetPlatformUserId();

	// Removal destroys the player controller, which runs the regular logout
	// path; the engine unregisters the player from any live session there.
	if (!GameInstance->RemoveLocalPlayer(PlayerToRemove))
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("RemoveLocalCoopPlayer failed: the engine refused to remove local player %d."), NumLocalPlayers - 1);
		return false;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Local co-op player removed: PlatformUser=%d released, %d local player(s) remain."),
		UserId.GetInternalId(), GameInstance->GetNumLocalPlayers());

	return true;
}

void UUSSessionSubsystem::AbortSession(const FString& Reason)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
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

	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
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

void UUSSessionSubsystem::HandleGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
{
	// The event is global and PIE runs several game instances side by side.
	if (GameMode == nullptr || NewPlayer == nullptr || GameMode->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	const APlayerState* PlayerState = NewPlayer->PlayerState;
	if (PlayerState == nullptr)
	{
		return;
	}

	// The player state's id is the right one for everyone: the authority copied it
	// from the local player for its own players, and read it off the wire for
	// remote ones. Both came from the same backend, because there is only one.
	TryRegisterPlayerInSession(PlayerState->GetUniqueId(),
		NewPlayer->IsLocalController() ? TEXT("a local login") : TEXT("a remote login"));
}

void UUSSessionSubsystem::RegisterLocalPlayersWithSession(const FString& Context)
{
	const UGameInstance* GameInstance = GetGameInstance();
	for (const ULocalPlayer* LocalPlayer : GameInstance->GetLocalPlayers())
	{
		if (LocalPlayer != nullptr)
		{
			TryRegisterPlayerInSession(LocalPlayer->GetPreferredUniqueNetId(), Context);
		}
	}
}

void UUSSessionSubsystem::TryRegisterPlayerInSession(const FUniqueNetIdRepl& UniqueId, const FString& Context)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
	const FNamedOnlineSession* Session = Sessions.IsValid() ? Sessions->GetNamedSession(NAME_GameSession) : nullptr;
	if (Session == nullptr)
	{
		// Pure couch co-op: no session, nothing to keep in sync.
		return;
	}

	if (!UniqueId.IsValid())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("Cannot register a player from %s: no valid unique net id. Check the identity setup of the active online backend."), *Context);
		return;
	}

	const FUniqueNetIdPtr PlayerId = UniqueId.GetUniqueNetId();

	// The engine registers players itself whenever its timing works out (see
	// APlayerState::RegisterPlayerWithSession, which now finds this very session
	// because it goes through the default subsystem); this only fills the gap it
	// leaves when the login ran before the world switched to a networked mode.
	// Checking first is what keeps the two from fighting: both backends dedupe by
	// id anyway, but skipping the call keeps the log honest about who registered
	// what.
	const bool bAlreadyRegistered = Session->RegisteredPlayers.ContainsByPredicate(
		[&PlayerId](const FUniqueNetIdRef& Registered)
		{
			return *Registered == *PlayerId;
		});
	if (bAlreadyRegistered)
	{
		return;
	}

	EnsureRegisterPlayersDelegate();

	if (Sessions->RegisterPlayer(NAME_GameSession, *PlayerId, /*bWasInvited*/ false))
	{
		// Accepted, not finished: the outcome arrives on OnRegisterPlayersComplete.
		UE_LOG(LogUnrealSplit, Log, TEXT("Registering player '%s' in the session (%s)."), *UniqueId.ToDebugString(), *Context);
	}
	else
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("The backend refused to register player '%s' in the session (%s)."), *UniqueId.ToDebugString(), *Context);
	}
}

void UUSSessionSubsystem::EnsureRegisterPlayersDelegate()
{
	if (RegisterPlayersCompleteHandle.IsValid())
	{
		return;
	}

	const IOnlineSessionPtr Sessions = GetSessionInterface(this);
	if (!Sessions.IsValid())
	{
		return;
	}

	// Bound for the lifetime of the subsystem: it also reports the registrations
	// the engine itself starts, which is exactly the coverage we want.
	RegisterPlayersCompleteHandle = Sessions->AddOnRegisterPlayersCompleteDelegate_Handle(
		FOnRegisterPlayersCompleteDelegate::CreateWeakLambda(this, [](FName SessionName, const TArray<FUniqueNetIdRef>& PlayerIds, bool bWasSuccessful)
		{
			if (SessionName != FName(NAME_GameSession))
			{
				return;
			}

			if (bWasSuccessful)
			{
				UE_LOG(LogUnrealSplit, Log, TEXT("Session registration confirmed for %d player(s)."), PlayerIds.Num());
			}
			else
			{
				UE_LOG(LogUnrealSplit, Warning, TEXT("Session registration failed for %d player(s); the session's open slots and membership no longer match the players in the game."), PlayerIds.Num());
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

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(this))
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

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(this))
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

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(this))
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

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(this))
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteHandle);
	}
	DestroySessionCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearRegisterPlayersDelegate()
{
	if (!RegisterPlayersCompleteHandle.IsValid())
	{
		return;
	}

	if (const IOnlineSessionPtr Sessions = GetSessionInterface(this))
	{
		Sessions->ClearOnRegisterPlayersCompleteDelegate_Handle(RegisterPlayersCompleteHandle);
	}
	RegisterPlayersCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearLoginDelegate()
{
	if (!LoginCompleteHandle.IsValid())
	{
		return;
	}

	const IOnlineIdentityPtr Identity = GetIdentityInterface(this);
	if (Identity.IsValid() && PendingLoginLocalUserIndex != INDEX_NONE)
	{
		// Takes the handle by reference and resets it.
		Identity->ClearOnLoginCompleteDelegate_Handle(PendingLoginLocalUserIndex, LoginCompleteHandle);
	}
	LoginCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearBackendLoginDelegate()
{
	if (!BackendLoginCompleteHandle.IsValid())
	{
		return;
	}

	const IOnlineIdentityPtr Identity = GetIdentityInterface(this);
	if (Identity.IsValid())
	{
		// Takes the handle by reference and resets it.
		Identity->ClearOnLoginCompleteDelegate_Handle(SessionLocalUserNum, BackendLoginCompleteHandle);
	}
	BackendLoginCompleteHandle.Reset();
}

void UUSSessionSubsystem::ClearSessionDelegates()
{
	ClearCreateSessionDelegate();
	ClearFindSessionsDelegate();
	ClearJoinSessionDelegate();
	ClearDestroySessionDelegate();
	ClearRegisterPlayersDelegate();
	ClearLoginDelegate();
	ClearBackendLoginDelegate();
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
