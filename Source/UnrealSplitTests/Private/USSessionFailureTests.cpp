// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "USSessionSubsystem.h"
#include "USSessionTestHelpers.h"
#include "USSessionTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Group 5 — failure & abort paths reachable without runtime seams.
 *
 * Not covered (Null rarely fails; no test seam): CreateSession callback failure,
 * DestroySession failure. Join invalid/stale is covered. Create/destroy natural
 * failure stays manual / future seam if ever approved.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionFailureJoinInvalidIndexTest,
	"UnrealSplit.Session.Failure.JoinInvalidIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionFailureJoinInvalidIndexTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return false;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	// Ensure Idle before join attempt.
	if (Sessions->GetSessionState() != EUSSessionState::Idle)
	{
		AddWarning(TEXT("Could not reach Idle before JoinInvalidIndex."));
		return true;
	}

	BindJoinResult(Context);
	Sessions->JoinFoundSession(0);
	TestTrue(TEXT("Join invalid index fires join delegate."), Context->JoinCallbackSeen());
	TestFalse(TEXT("Join invalid index fails cleanly."), Context->JoinSucceeded());
	TestEqual(TEXT("State remains Idle."), Sessions->GetSessionState(), EUSSessionState::Idle);
	TestFalse(TEXT("No OSS session after failed join."), HasNamedGameSession(Sessions));
	AssertInvariants(*this, Sessions, TEXT("JoinInvalidIndex"));
	Context->ClearDelegates();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionFailureJoinStaleAfterNoSearchTest,
	"UnrealSplit.Session.Failure.JoinStaleNoSearch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionFailureJoinStaleAfterNoSearchTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return false;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle)
	{
		AddWarning(TEXT("Could not reach Idle before JoinStaleNoSearch."));
		return true;
	}

	TestEqual(TEXT("No cached search results."), Sessions->GetFoundSessions().Num(), 0);

	BindJoinResult(Context);
	Sessions->JoinFoundSession(99);
	TestTrue(TEXT("Stale join fires join delegate."), Context->JoinCallbackSeen());
	TestFalse(TEXT("Stale join fails."), Context->JoinSucceeded());
	TestEqual(TEXT("Idle after stale join."), Sessions->GetSessionState(), EUSSessionState::Idle);
	AssertInvariants(*this, Sessions, TEXT("JoinStale"));
	Context->ClearDelegates();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
