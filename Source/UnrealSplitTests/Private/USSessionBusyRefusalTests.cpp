// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "USSessionSubsystem.h"
#include "USSessionTestHelpers.h"
#include "USSessionTestListener.h"
#include "USSessionTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Group 2 — busy-state refusals (Null, single process).
 *
 * Skipped by design (need InSessionAsClient / second process):
 *   Host while joined, Join while already joined.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionBusyLeaveWhileIdleTest,
	"UnrealSplit.Session.Busy.LeaveWhileIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionBusyLeaveWhileIdleTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
		ArmDeadline(Context, 30.0);
		ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	}

	// Capture state after any cleanup via a second latent that runs Leave on Idle.
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));

	struct FAssertIdleNoop : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FAssertIdleNoop(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}
			FAutomationTestBase& Test = *Ctx->Test;
			Test.TestEqual(TEXT("Leave while Idle stays Idle."), Ctx->Sessions->GetSessionState(), EUSSessionState::Idle);
			Test.TestFalse(TEXT("Leave while Idle creates no OSS session."), HasNamedGameSession(Ctx->Sessions.Get()));
			AssertInvariants(Test, Ctx->Sessions.Get(), TEXT("LeaveWhileIdle"));
			Ctx->ClearDelegates();
			return true;
		}
	};
	ADD_LATENT_AUTOMATION_COMMAND(FAssertIdleNoop(Context));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionBusyHostWhileHostingTest,
	"UnrealSplit.Session.Busy.HostWhileHosting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionBusyHostWhileHostingTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	BindHostResult(Context);
	ArmDeadline(Context, 30.0);
	Sessions->HostSession(2);

	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));

	struct FSecondHostRefused : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FSecondHostRefused(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}

			FAutomationTestBase& Test = *Ctx->Test;
			UUSSessionSubsystem* const SessionsInner = Ctx->Sessions.Get();
			const int32 RegisteredBefore = GetOssRegisteredPlayerCount(SessionsInner);

			UUSSessionTestListener* const Listener = Ctx->EnsureListener();
			Listener->ResetHostFlags();
			if (!Listener->bHostBound)
			{
				Listener->BindHost(SessionsInner);
			}

			SessionsInner->HostSession(2);

			Test.TestTrue(TEXT("Second HostSession should report via delegate."), Listener->bHostSeen);
			Test.TestFalse(TEXT("Host while hosting must fail."), Listener->bHostOk);
			Test.TestEqual(TEXT("Still Hosting after refused second host."), SessionsInner->GetSessionState(), EUSSessionState::Hosting);
			Test.TestTrue(TEXT("Original OSS session untouched."), HasNamedGameSession(SessionsInner));
			Test.TestEqual(TEXT("Registered player count unchanged by refused host."),
				GetOssRegisteredPlayerCount(SessionsInner), RegisteredBefore);
			AssertInvariants(Test, SessionsInner, TEXT("HostWhileHosting"));
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FSecondHostRefused(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionBusyJoinWhileHostingTest,
	"UnrealSplit.Session.Busy.JoinWhileHosting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionBusyJoinWhileHostingTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	BindHostResult(Context);
	ArmDeadline(Context, 30.0);
	Sessions->HostSession(2);

	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));

	struct FJoinWhileHosting : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FJoinWhileHosting(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}

			FAutomationTestBase& Test = *Ctx->Test;
			UUSSessionSubsystem* const SessionsInner = Ctx->Sessions.Get();

			BindJoinResult(Ctx);
			SessionsInner->JoinFoundSession(0);

			Test.TestTrue(TEXT("Join while hosting should complete the join delegate."), Ctx->JoinCallbackSeen());
			Test.TestFalse(TEXT("Join while hosting must fail."), Ctx->JoinSucceeded());
			Test.TestEqual(TEXT("Host state preserved."), SessionsInner->GetSessionState(), EUSSessionState::Hosting);
			Test.TestTrue(TEXT("Host OSS session preserved."), HasNamedGameSession(SessionsInner));
			AssertInvariants(Test, SessionsInner, TEXT("JoinWhileHosting"));
			Ctx->ClearDelegates();
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FJoinWhileHosting(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionBusyHostReentrancyNeverTwoSessionsTest,
	"UnrealSplit.Session.Busy.HostReentrancyNeverTwoSessions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionBusyHostReentrancyNeverTwoSessionsTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	// Null often completes CreateSession synchronously, so mid-Creating is not
	// observable between calls. Back-to-back HostSession still asserts: never two
	// NAME_GameSession entries / second call refused once non-Idle.
	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	BindHostResult(Context);
	Sessions->HostSession(2);
	Sessions->HostSession(2);

	ArmDeadline(Context, 30.0);
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));

	struct FAssertSingleSession : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FAssertSingleSession(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}
			FAutomationTestBase& Test = *Ctx->Test;
			Test.TestTrue(TEXT("Exactly one named game session exists."), HasNamedGameSession(Ctx->Sessions.Get()));
			Test.TestEqual(TEXT("State is Hosting after double HostSession."), Ctx->Sessions->GetSessionState(), EUSSessionState::Hosting);
			AssertInvariants(Test, Ctx->Sessions.Get(), TEXT("HostReentrancy"));
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FAssertSingleSession(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
