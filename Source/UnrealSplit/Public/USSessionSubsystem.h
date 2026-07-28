// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Net/Core/Connection/NetEnums.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USSessionTypes.h"
#include "USSessionSubsystem.generated.h"

class FOnlineSessionSearch;
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
 * Every public entry point is guarded by EUSSessionState and always resolves its
 * delegate — a rejected call broadcasts failure rather than leaving a caller
 * waiting. Network and travel failures tear the session down and return to the
 * entry map, so a dropped connection cannot leave a ghost session behind.
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
	 * @param MaxPlayers Public connections to advertise; clamped to at least 1.
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
	 * Joins a previously found session and travels to the host.
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

	void ReturnToEntryMap();

	void ClearCreateSessionDelegate();
	void ClearFindSessionsDelegate();
	void ClearJoinSessionDelegate();
	void ClearDestroySessionDelegate();
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

	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
};
