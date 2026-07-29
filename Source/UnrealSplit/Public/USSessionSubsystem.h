// Copyright (c) Jonas Itt. All Rights Reserved.

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
struct FUniqueNetIdRepl;
class UNetDriver;
class UWorld;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnSessionHosted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnSessionsFound, int32, NumResults);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnJoinCompleted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUSOnSessionEnded, FString, Reason);

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
 * Session mode is deliberately invisible to the view system — both modes render
 * two view slots — so this subsystem also owns setting up the shadow view that
 * gives an online client a view of the remote player. How that view is produced
 * is still open; it is selected by the comparison spike in plan step 1.6.
 *
 * Backend-agnostic by construction: everything goes through the generic
 * IOnlineSession interface, and no Online Subsystem type appears in this header.
 * Which backend is used is a config decision (DefaultPlatformService), so moving
 * from Null/LAN to EOS or Steam requires no change here beyond bUseLANMatch.
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
	 * Creates an advertised session and, on success, reopens the current level as
	 * a listen server. Broadcasts OnSessionHostedDelegate either way.
	 * @param MaxPlayers Public connections to advertise, counting the host's own
	 *   local players; clamped to 1..MaxTotalPlayers so the session can never
	 *   advertise a seat the game would refuse to fill.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Session")
	void HostSession(int32 MaxPlayers = 2);

	/**
	 * Searches for joinable sessions. Results are cached on the subsystem and
	 * reported through OnSessionsFoundDelegate; read them with GetFoundSessions.
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

protected:
	/**
	 * Restrict discovery to the local network. True for the Null/LAN development
	 * path; set to false in config once an internet backend (EOS/Steam) is live.
	 */
	UPROPERTY(Config)
	bool bUseLANMatch = true;

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
	void ClearRegisterPlayersDelegate();
	void ClearLoginDelegate();
	void ClearSessionDelegates();

	void SetSessionState(EUSSessionState NewState);

	EUSSessionState SessionState = EUSSessionState::Idle;

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

	/** Platform user whose login AddLocalCoopPlayer is waiting for, if any. */
	int32 PendingLoginLocalUserIndex = INDEX_NONE;
	FPlatformUserId PendingLoginPlatformUser = PLATFORMUSERID_NONE;

	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FDelegateHandle RegisterPlayersCompleteHandle;
	FDelegateHandle LoginCompleteHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoginHandle;
};
