// Copyright (c) Jonas Itt. All Rights Reserved.

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
 * Exactly one mode is active at a time and it is chosen before anything is
 * hosted or joined; there is never a game whose players arrived by different
 * routes. Pure couch co-op is deliberately absent: it uses no session and no
 * online backend at all, so it is the absence of this choice rather than a
 * third value of it.
 *
 * Each mode names an online subsystem through config (UUSSessionSubsystem's
 * LanSubsystemName / InternetSubsystemName), so no backend name ever appears in
 * plugin code and swapping either one is an ini edit.
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

	/** Display name of the hosting player, as reported by the backend. */
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
};
