// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "USSessionSubsystem.h"
#include "USSessionTestHelpers.h"
#include "USSessionTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Group 1 — lifecycle happy paths (Null, single process).
 * Host triggers listen-server OpenLevel; waits are latent.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionLifecycleHostSucceedsTest,
	"UnrealSplit.Session.Lifecycle.HostSucceeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionLifecycleHostSucceedsTest::RunTest(const FString& Parameters)
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
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertHostingInvariants(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionLifecycleHostLeaveIdleTest,
	"UnrealSplit.Session.Lifecycle.HostLeaveIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionLifecycleHostLeaveIdleTest::RunTest(const FString& Parameters)
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
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionLifecycleHostLeaveHostAgainTest,
	"UnrealSplit.Session.Lifecycle.HostLeaveHostAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionLifecycleHostLeaveHostAgainTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	// Mandatory cycle: Host → Leave → Host. Catches stale-flag / half-open state.
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
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentHostAgain(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertHostingInvariants(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
