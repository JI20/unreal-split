// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Net/Core/Connection/NetEnums.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USSessionTypes.h"
#include "USSessionSubsystem.generated.h"

class AGameModeBase;
class APlayerController;
class FOnlineSessionSearch;
class FOnlineSessionSettings;
struct FUniqueNetIdRepl;
class UNetDriver;
class UWorld;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnSessionHosted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnSessionsFound, int32, NumResults);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnJoinCompleted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnSessionEnded, FString, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnBackendReady, bool, bReady);

/**
 * Connection layer (host/join/travel) for UnrealSplit.
 *
 * Owns axis 1 of the UnrealSplit view model: whether the session runs as local
 * splitscreen or over the network. Game-instance scoped so it survives level
 * travel, which any session flow depends on.
 *
 * Also owns local co-op membership (AddLocalCoopPlayer / RemoveLocalCoopPlayer):
 * couch players joining or leaving are session-membership events exactly like
 * remote clients, and keeping both here gives one place that reconciles them
 * with the online session (player registration, capacity). Capacity is one
 * number for both kinds: MaxTotalPlayers caps local and remote players together,
 * so which mix fills the game is a config decision, not a hard-coded one.
 *
 * Session mode is deliberately invisible to the view system: every machine renders
 * two view slots, and online the second one is filled by UUSRemoteViewSubsystem
 * from the partner's replicated camera. That subsystem is separate from this one
 * because it answers a local rendering question, not a membership question; what
 * this subsystem owes it is only that a shadow view never counts as a player, so
 * every seat, capacity and registration decision here counts real local players
 * (UUSShadowLocalPlayer::CountRealLocalPlayers) rather than views.
 *
 * Backend-agnostic by construction: everything goes through the generic
 * IOnlineSession interface of the process-default online subsystem, and no
 * Online Subsystem type appears in this header. Which backend that is, is not a
 * runtime choice — UnrealSplitStartup selects it once from -ConnectionMode=
 * before the engine reads it — so EUSConnectionMode is a read-only fact about how
 * this process was launched, and every call here is a plain default-subsystem
 * call. Two things follow from the default being the active backend. Local
 * players' net ids are natively the type that backend expects, because
 * ULocalPlayer mints them from the default subsystem; the plugin therefore needs
 * no id translation anywhere. And engine code that reaches for the default on its
 * own — most visibly APlayerState::RegisterPlayerWithSession — talks to the same
 * session this subsystem does, so its registration and this one's agree instead
 * of one of them quietly doing nothing.
 *
 * Every session entry point is guarded by EUSSessionState and always resolves
 * its delegate — a rejected call broadcasts failure rather than leaving a caller
 * waiting. Network and travel failures tear the session down and return to the
 * entry map, so a dropped connection cannot leave a ghost session behind. The
 * local co-op entry points are guarded by capacity and net mode instead, and
 * report through their return value and the log rather than a delegate.
 */
UCLASS(Config = Engine)
class UNREALSPLIT_API UUSSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/**
	 * How players reach each other in this process, fixed at launch. Local co-op
	 * works under either mode, so this says nothing about whether a session exists.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	EUSConnectionMode GetConnectionMode() const { return ConnectionMode; }

	/**
	 * Re-registers this world's players in the session after a seamless travel.
	 *
	 * A named session outlives a world, and nothing in a seamless travel logs a
	 * player out, so in the ordinary case every registration made at login still
	 * stands and this call finds nothing to do. It runs anyway because the
	 * ordinary case is the only one the engine guarantees: HandleSeamlessTravelPlayer
	 * may put a player on a freshly spawned controller, AGameSession::PostSeamlessTravel
	 * is empty, and the post-login event this subsystem normally registers from
	 * never fires for a carried player. Registering again is idempotent — the
	 * backend answers a repeat with "already registered" — so the reconcile costs
	 * one call per player per travel and removes the assumption.
	 *
	 * Authority only, matching the post-login path. A client registered itself when
	 * it joined and that registration is not world-scoped.
	 */
	void ReregisterPlayersAfterSeamlessTravel();

	/** Online subsystem this process runs on, e.g. for a status line. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	FName GetBackendName() const;

	/** Whether that backend came up at all; false means no session can be hosted or joined. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	bool IsBackendInstalled() const;

	/**
	 * Whether the backend will accept session calls: either the mode needs no
	 * login, or the primary user is logged in.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	bool IsBackendReady() const;

	/**
	 * The same answer with the reason attached. Worth reading instead of
	 * IsBackendReady wherever a failure is reported to a person: an unsigned-in
	 * process searches and finds nothing, which looks exactly like a search that
	 * worked and came back empty, and the two need completely different fixes.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	EUSBackendStatus GetBackendStatus() const;

	/**
	 * Whether sessions in this mode are advertised as lobbies rather than as
	 * sessions proper. Both host and search read it, which is the point: on a
	 * hosted service the two are separate registries, and a host in one is
	 * invisible to a search of the other.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	bool DoesModeUseLobbies() const;

	/**
	 * Key the backend files this game's sessions under. Both host and search
	 * send it and a hosted service only ever matches within one bucket, so a
	 * host and a searcher that disagree about it never see each other.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	FString GetSessionBucketId() const;

	/**
	 * Both discovery keys on one line, in the same wording the host and search
	 * logs use. What a status readout should show, and what two processes are
	 * compared by when one cannot see the other.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	FString DescribeDiscoveryKeys() const;

	/**
	 * Logs the primary user into the backend. Started on the first session call
	 * that needs it already; exposed for a menu that wants a retry button after a
	 * failed login.
	 * @return Whether a login was started or was unnecessary. The outcome arrives
	 *   through OnBackendReadyDelegate.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	bool LoginToBackend();

	/**
	 * Creates an advertised session and, on success, opens a listen server.
	 *
	 * With TravelToMap empty, the current level is reopened — the original host
	 * path, and what tests still use. A non-empty map is opened instead, so a
	 * title screen can host straight into a lobby without becoming the listen
	 * server itself. Broadcasts OnSessionHostedDelegate either way.
	 *
	 * @param MaxPlayers Public connections to advertise, counting the host's own
	 *   local players; clamped to 1..MaxTotalPlayers so the session can never
	 *   advertise a seat the game would refuse to fill.
	 * @param TravelToMap Package name or path to open as the listen server, or
	 *   empty to reopen the current level.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	void HostSession(int32 MaxPlayers = 2, const FString& TravelToMap = TEXT(""));

	/**
	 * Writes a Find-list snapshot onto the hosted session.
	 *
	 * Host only. Cached immediately; pushed to the backend while Hosting, or
	 * stamped onto the next CreateSession while Idle/Creating. Clients are
	 * refused — they cannot advertise on the host's session.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	void SetAdvertisedSessionState(const FUSAdvertisedSessionState& State);

	/** The snapshot this machine last wrote, or default if it never has. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	FUSAdvertisedSessionState GetAdvertisedSessionState() const { return AdvertisedSessionState; }

	/** Blueprint entry for FUSAdvertisedSessionState::GetFindListSummary (UHT forbids UFUNCTION on structs). */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	static FString GetAdvertisedFindListSummary(const FUSAdvertisedSessionState& State)
	{
		return State.GetFindListSummary();
	}

	/** Blueprint entry for FUSAdvertisedSessionState::GetVacantLabels. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	static TArray<FString> GetAdvertisedVacantLabels(const FUSAdvertisedSessionState& State)
	{
		return State.GetVacantLabels();
	}

	/**
	 * Searches for joinable sessions. Results are cached on the subsystem and
	 * reported through OnSessionsFoundDelegate; read them with GetFoundSessions.
	 *
	 * A refused search reports zero results just like a search that found
	 * nothing, so anything that shows the outcome to a person must read
	 * GetBackendStatus() as well and say "not signed in" when that is what
	 * happened. The log always distinguishes the two.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	void FindSessions();

	/**
	 * Joins a previously found session and travels to the host. Refused when this
	 * machine's local players would not fit in the session's free seats, since
	 * every local player joins with it.
	 * @param SearchResultIndex Index into GetFoundSessions(), not the raw backend result list.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	void JoinFoundSession(int32 SearchResultIndex);

	/**
	 * Destroys the session and returns to the entry map. Safe for both host and
	 * client, and safe to call with no session active.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	void LeaveSession();

	/**
	 * Takes everyone in this session to another map, keeping the session alive.
	 *
	 * The forward counterpart to the entry-map return: that one ends a session and
	 * goes back, this one carries it onward, and both exist so a game never has to
	 * reach past the plugin for travel and get the listen server or the local
	 * players wrong.
	 *
	 * Gated on being the authority rather than on holding a session, because couch
	 * co-op has no session at all and still has to travel the same way — standalone
	 * is its own authority and a server travel carries every local player with it.
	 * A client is refused: the host decides where the session goes.
	 *
	 * @param MapName Package name or path of the destination map.
	 * @return Whether the travel was started.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	bool TravelSessionToMap(const FString& MapName);

	/**
	 * Adds a couch co-op player on this machine. The new player gets the next
	 * platform user, which on desktop pairs it with the matching gamepad while
	 * keyboard/mouse stay with player 1. That user is logged into the online
	 * backend first, because the engine derives the player state's unique net id
	 * from the local player when it logs in; a player created without an identity
	 * could never be registered in the session.
	 *
	 * Refused when the game is already at MaxTotalPlayers — which counts remote
	 * players, so a client connected to a host that fills the game cannot add a
	 * second player here — and while a session operation is in flight.
	 *
	 * @return True when the player was added, which is the normal case: backends
	 *   that log in synchronously (Null/LAN) finish inside this call. Returns
	 *   false when the request was refused *or* when it is waiting on an
	 *   asynchronous login, in which case the player appears once that resolves.
	 *   The log distinguishes the two.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	bool AddLocalCoopPlayer();

	/**
	 * Removes the most recently added couch co-op player. Never removes the
	 * primary player, and is refused while connected to a remote host because
	 * the engine cannot tell the server to drop the player cleanly.
	 * @return Whether a player was removed.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	bool RemoveLocalCoopPlayer();

	/** Current state of the connection layer. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	EUSSessionState GetSessionState() const { return SessionState; }

	/** Results of the last completed search, in the order they were found. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Session")
	TArray<FUSSessionSearchResultInfo> GetFoundSessions() const { return FoundSessions; }

	/** Fired when HostSession resolves, successfully or not. */
	UPROPERTY(BlueprintAssignable, Category = "UnrealSplit|Session")
	FUSOnSessionHosted OnSessionHostedDelegate;

	/** Fired when a search resolves, with the number of usable results. */
	UPROPERTY(BlueprintAssignable, Category = "UnrealSplit|Session")
	FUSOnSessionsFound OnSessionsFoundDelegate;

	/** Fired when JoinFoundSession resolves, successfully or not. */
	UPROPERTY(BlueprintAssignable, Category = "UnrealSplit|Session")
	FUSOnJoinCompleted OnJoinCompletedDelegate;

	/** Fired whenever a session ends, whether requested or caused by a failure. */
	UPROPERTY(BlueprintAssignable, Category = "UnrealSplit|Session")
	FUSOnSessionEnded OnSessionEndedDelegate;

	/**
	 * Fired when the selected backend becomes usable, or fails to. A menu should
	 * keep host/join disabled until this reports true.
	 */
	UPROPERTY(BlueprintAssignable, Category = "UnrealSplit|Session")
	FUSOnBackendReady OnBackendReadyDelegate;

protected:
	/**
	 * Whether a mode needs a logged-in user before session calls. LAN backends
	 * generally do not; hosted services do, and their net driver needs the login
	 * as well to build a connect string. A mode that needs one refuses to host,
	 * search or join until it has it.
	 */
	UPROPERTY(Config)
	bool bLanRequiresLogin = false;

	UPROPERTY(Config)
	bool bInternetRequiresLogin = true;

	/**
	 * Advertise internet sessions as lobbies where the backend supports them.
	 * Lobbies are how most hosted services let a client join without a routable
	 * host address; LAN never uses them.
	 *
	 * Read by the host *and* by the search, and it has to stay that way. A
	 * hosted service keeps lobbies and sessions in separate registries and
	 * answers a query of the wrong one with an empty list rather than an error,
	 * so two processes configured differently here simply never see each other.
	 */
	UPROPERTY(Config)
	bool bInternetUsesLobbies = true;

	/**
	 * Map to return to when a session ends. Falls back to the project's default
	 * map when empty, since the plugin cannot know the game's menu level.
	 */
	UPROPERTY(Config)
	FString EntryMapOverride;

	/**
	 * How many players may be in the game at once, counting local and remote
	 * together. Two-player co-op means any mix that adds up to it is valid — two
	 * on one machine, or one each on two machines — and nothing beyond it is.
	 *
	 * This is the plugin's own guard, which refuses over-capacity requests early
	 * and with a clear log. The authoritative one is the engine's
	 * AGameSession::MaxPlayers (globalconfig), which every login path checks; the
	 * two are meant to be configured to the same number.
	 */
	UPROPERTY(Config)
	int32 MaxTotalPlayers = 2;

private:
	/**
	 * Completion handlers, kept free of Online Subsystem types on purpose.
	 *
	 * Each takes the id of the operation it belongs to, because a backend is
	 * allowed to fire a completion synchronously and still report failure from the
	 * call itself. Only the handler invocation that owns the id does the work.
	 */
	void OnCreateSessionFinished(uint32 OperationId, bool bWasSuccessful);
	void OnFindSessionsFinished(uint32 OperationId, bool bWasSuccessful);
	void OnJoinSessionFinished(uint32 OperationId, bool bSuccess, const FString& ResultDescription);
	void OnDestroySessionFinished(uint32 OperationId, bool bWasSuccessful);
	void OnUpdateSessionFinished(FName SessionName, bool bWasSuccessful);

	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString);

	/** Destroys any live session, returns to the entry map and reports the reason. */
	void AbortSession(const FString& Reason);

	/**
	 * Claims the next async operation. Doing so abandons anything still in flight:
	 * its completion no longer owns the state machine and cleans up after itself.
	 */
	uint32 BeginOperation();

	/** True while OperationId is the completion this subsystem is still waiting for. */
	bool IsOperationPending(uint32 OperationId) const;

	/**
	 * Takes ownership of OperationId's completion. Returns false when that
	 * completion already ran or the operation was abandoned, which is what makes
	 * the handlers safe to reach from both the delegate and the calling code.
	 */
	bool ConsumeOperation(uint32 OperationId);

	/** Drops a session the backend registered but that never became usable. */
	void DestroyLeftoverSession(const FString& Context);

	/**
	 * Authority-side registration: fires for every login on a listen server —
	 * the host's own players, couch players added later, and remote clients —
	 * after the engine's own registration attempt, so this only fills the gap
	 * the engine leaves when the world was still standalone at login time.
	 *
	 * Unregistration is deliberately absent. The engine already unregisters on
	 * logout (AGameSession::NotifyLogout) *after* broadcasting the logout event,
	 * so a plugin-side unregister would always run first and turn the engine's
	 * call into the exact "player not in session" warning this fixes. Session
	 * teardown drops all registrations with the session itself.
	 */
	void HandleGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer);

	/** Registers every local player with the current session. Safe to repeat. */
	void RegisterLocalPlayersWithSession(const FString& Context);

	/** Idempotent RegisterPlayer: no session or already registered is a no-op. */
	void TryRegisterPlayerInSession(const FUniqueNetIdRepl& UniqueId, const FString& Context);

	/** Creates the local player for an already logged-in platform user. */
	bool CreateLocalCoopPlayerForUser(FPlatformUserId UserId);

	/** Whether the current mode may not touch the backend before a login. */
	bool DoesModeRequireLogin() const;

	/** True when discovery should stay on the local network. */
	bool IsLanMatch() const;

	/**
	 * Stamps the keys a backend matches a search against onto the host's settings
	 * and onto the search, from the same two answers on both sides.
	 *
	 * Deliberately one function per side of the same decision rather than two
	 * independent ones: session kind and bucket only work if host and searcher
	 * agree on them, and a hosted service reports a disagreement as an empty
	 * result rather than as an error. Keeping them here is what stops the two
	 * from drifting apart.
	 */
	void ApplyDiscoveryKeys(FOnlineSessionSettings& Settings) const;
	void ApplyDiscoveryKeys(FOnlineSessionSearch& Search) const;

	/** Stamps or clears the Find-list snapshot on session settings. */
	void ApplyAdvertisedSessionState(FOnlineSessionSettings& Settings) const;

	/** Reads a Find-list snapshot off search or live session settings. */
	static FUSAdvertisedSessionState ReadAdvertisedSessionState(const FOnlineSessionSettings& Settings);

	/** Pushes the cached snapshot to the backend. No-op when not Hosting. */
	void PushAdvertisedSessionStateToBackend();

	/**
	 * Common guard for the session entry points: reports whether the backend can
	 * be used, and starts the missing login when it cannot, so a refused call
	 * becomes a retry rather than a dead end.
	 */
	bool EnsureBackendReady(const TCHAR* Context);

	/** Result of the primary user's login into the selected backend. */
	void OnBackendLoginFinished(int32 LocalUserIndex, bool bWasSuccessful, const FString& ErrorString);

	void ClearBackendLoginDelegate();

	/**
	 * Players currently in the game as this machine can see them: everyone on the
	 * authority, everyone the game state knows on a client, and the local players
	 * before either exists. Basis for the MaxTotalPlayers guard.
	 */
	int32 GetKnownPlayerCount() const;

	/** Continues or abandons the add once the platform user's login resolves. */
	void OnLocalCoopPlayerLoginFinished(int32 LocalUserIndex, bool bWasSuccessful, const FString& ErrorString);

	/**
	 * Observes registration outcomes. RegisterPlayer returning true only means
	 * the backend accepted the request; on asynchronous backends the real result
	 * arrives here, and without it a failed registration would pass silently.
	 */
	void EnsureRegisterPlayersDelegate();

	void ReturnToEntryMap();

	void ClearCreateSessionDelegate();
	void ClearFindSessionsDelegate();
	void ClearJoinSessionDelegate();
	void ClearDestroySessionDelegate();
	void ClearUpdateSessionDelegate();
	void ClearRegisterPlayersDelegate();
	void ClearLoginDelegate();
	void ClearSessionDelegates();

	void SetSessionState(EUSSessionState NewState);

	EUSSessionState SessionState = EUSSessionState::Idle;

	/**
	 * How this process was launched, read once in Initialize from the backend
	 * UnrealSplitStartup selected. Never written again: changing it would not
	 * change the backend, which is fixed for the life of the process.
	 */
	EUSConnectionMode ConnectionMode = EUSConnectionMode::LAN;

	/** True while the primary user's login into that backend is in flight. */
	bool bBackendLoginInFlight = false;

	/**
	 * Identifies the async operation whose completion is still expected. Bumped
	 * when one starts and again when it resolves, so duplicate and abandoned
	 * completions can be told apart from the one that drives the state machine.
	 */
	uint32 PendingOperationId = 0;

	/** Backend search request; also the source of truth for join-by-index. */
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	TArray<FUSSessionSearchResultInfo> FoundSessions;

	/** Carried into OnDestroySessionFinished so the reason survives the async hop. */
	FString PendingSessionEndReason;

	/**
	 * Map to open as the listen server when HostSession finishes, or empty to
	 * reopen the current level. Cleared when the create completes or is abandoned.
	 */
	FString PendingHostTravelMap;

	/** Last snapshot the game asked to advertise. Cleared when the session ends. */
	FUSAdvertisedSessionState AdvertisedSessionState;

	/** True while UpdateSession for that snapshot is in flight. */
	bool bAdvertisementUpdateInFlight = false;

	/** True when a newer snapshot arrived while an update was still in flight. */
	bool bAdvertisementDirty = false;

	/** Platform user whose login AddLocalCoopPlayer is waiting for, if any. */
	int32 PendingLoginLocalUserIndex = INDEX_NONE;
	FPlatformUserId PendingLoginPlatformUser = PLATFORMUSERID_NONE;

	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FDelegateHandle UpdateSessionCompleteHandle;
	FDelegateHandle RegisterPlayersCompleteHandle;
	FDelegateHandle LoginCompleteHandle;
	FDelegateHandle BackendLoginCompleteHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoginHandle;
};
