// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

class UUSSessionSubsystem;
class UUSSessionTestListener;

/**
 * Shared helpers for UnrealSplit.Session.* automation (Null OSS, single process).
 * Never applies net emulation. Never modifies runtime plugin source.
 */
#if WITH_DEV_AUTOMATION_TESTS

namespace USSessionTest
{
	enum class EWaitResult : uint8
	{
		Pending,
		Success,
		Timeout,
		LostSubsystem
	};

	/** Per-test scratch pad for latent host/leave sequences. */
	struct FContext
	{
		TWeakObjectPtr<UUSSessionSubsystem> Sessions;
		FAutomationTestBase* Test = nullptr;
		double DeadlineSeconds = 0.0;
		TStrongObjectPtr<UUSSessionTestListener> Listener;

		void ClearDelegates();
		~FContext() { ClearDelegates(); }

		UUSSessionTestListener* EnsureListener();
		bool HostCallbackSeen() const;
		bool HostSucceeded() const;
		bool JoinCallbackSeen() const;
		bool JoinSucceeded() const;
	};

	using FContextPtr = TSharedPtr<FContext, ESPMode::ThreadSafe>;

	/**
	 * Subsystem of a live PIE/game world. Never creates a world or game instance:
	 * session flows travel (listen server), which only a real game world survives.
	 * Returns nullptr with a warning when nothing is running — callers skip then.
	 */
	UUSSessionSubsystem* ResolveSubsystem(FAutomationTestBase& Test, FContextPtr Context);

	IOnlineSessionPtr GetOssSessions(const UUSSessionSubsystem* Subsystem);

	bool HasNamedGameSession(const UUSSessionSubsystem* Subsystem);
	int32 GetOssRegisteredPlayerCount(const UUSSessionSubsystem* Subsystem);
	int32 GetOssNumPublicConnections(const UUSSessionSubsystem* Subsystem);

	/** Peel couch players down to the primary only. */
	bool EnsureSingleLocalPlayer(FAutomationTestBase& Test, UUSSessionSubsystem* Sessions);

	void RequestLeave(UUSSessionSubsystem* Sessions);

	/**
	 * Cross-cutting checks after a mutation. Safe when Idle with no session.
	 * Returns false if a hard invariant failed (also records Test errors).
	 */
	bool AssertInvariants(FAutomationTestBase& Test, UUSSessionSubsystem* Sessions, const TCHAR* ContextLabel);

	void BindHostResult(FContextPtr Context);
	void BindJoinResult(FContextPtr Context);

	void ArmDeadline(FContextPtr Context, double SecondsFromNow = 30.0);

	EWaitResult PollUntilHosting(FContextPtr Context);
	EWaitResult PollUntilIdleNoSession(FContextPtr Context);
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUSLatentWaitHosting, USSessionTest::FContextPtr, Context);
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUSLatentWaitIdle, USSessionTest::FContextPtr, Context);
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUSLatentLeave, USSessionTest::FContextPtr, Context);
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUSLatentAssertHostingInvariants, USSessionTest::FContextPtr, Context);
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUSLatentAssertIdleInvariants, USSessionTest::FContextPtr, Context);
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUSLatentHostAgain, USSessionTest::FContextPtr, Context);

#endif // WITH_DEV_AUTOMATION_TESTS
