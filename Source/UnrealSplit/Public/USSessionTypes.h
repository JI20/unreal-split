// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "USSessionTypes.generated.h"

/**
 * Lifecycle of the connection layer (axis 1 of the UnrealSplit model).
 *
 * Every public entry point on UUSSessionSubsystem is guarded against states it
 * cannot legally run from, so the state is the single source of truth for
 * "is a session operation already in flight".
 */
UENUM(BlueprintType)
enum class EUSSessionState : uint8
{
	/** No session exists and nothing is in flight. */
	Idle,

	/** CreateSession is in flight. */
	Creating,

	/** This machine owns the session and is running as a listen server. */
	Hosting,

	/** FindSessions is in flight. */
	Searching,

	/** JoinSession is in flight, or travel to the host has been requested. */
	Joining,

	/** Connected to a remote host's session. */
	InSessionAsClient,

	/** DestroySession is in flight; the session is being torn down. */
	Leaving
};

/**
 * How players reach each other when a session is used.
 *
 * A read-only fact about the process, not a setting: the backend is selected
 * once at startup by UnrealSplitStartup from `-ConnectionMode=`, and one process
 * only ever has one. Switching means relaunching, which is also the only honest
 * option — the engine caches the default online subsystem and mints player ids
 * from it, so a live switch would leave both behind.
 *
 * Pure couch co-op is deliberately absent: it uses no session and no online
 * backend at all, so it works under either value and is the absence of this
 * choice rather than a third one.
 */
UENUM(BlueprintType)
enum class EUSConnectionMode : uint8
{
	/** Local network discovery, no accounts and no internet. */
	LAN,

	/** Internet play through a hosted online service. */
	Internet
};

/**
 * Whether the backend will accept a session call, and why not when it will not.
 *
 * "Not signed in" and "signed in but found nothing" are completely different
 * failures that both end in an empty session list, so the difference has to be
 * a value a caller can read rather than something only the log knows.
 */
UENUM(BlueprintType)
enum class EUSBackendStatus : uint8
{
	/** No online subsystem came up for this process's mode. Nothing online works. */
	NotInstalled,

	/** The backend is up, but the mode needs a signed-in user and there is none. */
	NotLoggedIn,

	/** A login is in flight; the answer arrives on OnBackendReadyDelegate. */
	LoggingIn,

	/** Session calls will be accepted: signed in, or the mode needs no sign-in. */
	Ready
};

/**
 * What a Find row can say about a session without joining it.
 *
 * Unknown is "this host never wrote anything" (or an older build). Lobby means
 * roles are not locked. InProgress means Start has written view slots, so a
 * vacant slot is a missing character.
 */
UENUM(BlueprintType)
enum class EUSAdvertisedSessionPhase : uint8
{
	Unknown,
	Lobby,
	InProgress
};

/** One view slot as advertised to searchers. Index in the parent array is the slot. */
USTRUCT(BlueprintType)
struct FUSAdvertisedSlot
{
	GENERATED_BODY()

	/** Game-supplied name for this half of the screen, e.g. Elmar. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	FString Label;

	/** True when a live player currently occupies this slot. Ignored while Lobby. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	bool bOccupied = false;
};

/**
 * Host-written snapshot of a session for the Find list.
 *
 * UnrealSplit stores this as a few reserved online-session keys; it does not
 * know character types. Labels come from the game. Extra is an escape hatch
 * for one-off strings and is not the place to put Elmar or Kit.
 */
USTRUCT(BlueprintType)
struct UNREALSPLIT_API FUSAdvertisedSessionState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	EUSAdvertisedSessionPhase Phase = EUSAdvertisedSessionPhase::Unknown;

	/** View slots, in slot order. Empty when the host never wrote a snapshot. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	TArray<FUSAdvertisedSlot> Slots;

	/** Optional game-specific strings. Not used for character identity. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	TMap<FString, FString> Extra;

	/** Labels of slots that are empty while InProgress. Empty for Lobby / Unknown. */
	TArray<FString> GetVacantLabels() const;

	/**
	 * One line for a Find row: "Lobby", "Kit missing", "In progress", or empty
	 * when the host wrote nothing (fall back to session id).
	 *
	 * Not a UFUNCTION: UHT forbids those on structs. Blueprint uses
	 * UUSSessionSubsystem::GetAdvertisedFindListSummary.
	 */
	FString GetFindListSummary() const;

	void Pack(int32& OutPhase, FString& OutLabels, int32& OutOccupied, FString& OutExtra) const;
	static FUSAdvertisedSessionState Unpack(int32 Phase, const FString& Labels, int32 Occupied, const FString& Extra);
};

/**
 * Backend-agnostic view of one session search result.
 *
 * Deliberately free of any Online Subsystem types so UI and Blueprint can show
 * search results without depending on which backend produced them.
 */
USTRUCT(BlueprintType)
struct FUSSessionSearchResultInfo
{
	GENERATED_BODY()

	/** Index into the underlying backend search results; pass this to JoinFoundSession. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	int32 SearchResultIndex = INDEX_NONE;

	/**
	 * The backend's own id for this session, as a string. Diagnostic rather than
	 * display: it is the one field that ties a result in this list to a line in
	 * the host's log, which is what tells "found the wrong session" apart from
	 * "found the right one and could not join it".
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	FString SessionId;

	/**
	 * Display name of the hosting player, as reported by the backend. Empty on
	 * backends that do not resolve owner names into search results — EOS lobby
	 * results are the case here — so never the only thing shown for a result.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	FString OwnerName;

	/** Round-trip time to the host in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	int32 PingInMs = 0;

	/** Public slots still free on the host. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	int32 OpenPublicConnections = 0;

	/** Public slots the host advertised in total. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	int32 MaxPublicConnections = 0;

	/**
	 * What the host last wrote for the Find list. Phase Unknown means this build
	 * of the host did not advertise a snapshot; Join still works.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Session")
	FUSAdvertisedSessionState Advertised;
};
