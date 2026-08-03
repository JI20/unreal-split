// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "USSessionSubsystem.h"
#include "USSessionTestHelpers.h"
#include "USSessionTypes.h"
#include "USShadowLocalPlayer.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Group 3 — capacity guards (MaxTotalPlayers = 2), local-only single process.
 * Remote+local combined capacity stays in the manual pair protocol.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionCapacityTwoLocalsRefuseThirdTest,
	"UnrealSplit.Session.Capacity.TwoLocalsRefuseThird",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionCapacityTwoLocalsRefuseThirdTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return false;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	UGameInstance* const GameInstance = Sessions->GetGameInstance();
	const bool bAddedSecond = Sessions->AddLocalCoopPlayer();
	if (!bAddedSecond)
	{
		AddWarning(TEXT("AddLocalCoopPlayer failed to add a second local (input/platform user?)."));
		return true;
	}

	TestEqual(TEXT("Two real locals after successful add."),
		UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 2);

	const bool bAddedThird = Sessions->AddLocalCoopPlayer();
	TestFalse(TEXT("Third local must be refused at MaxTotalPlayers."), bAddedThird);
	TestEqual(TEXT("Still two locals after refused third."),
		UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 2);
	AssertInvariants(*this, Sessions, TEXT("CapacityRefuseThird"));

	TestTrue(TEXT("Cleanup: remove secondary."), Sessions->RemoveLocalCoopPlayer());
	TestEqual(TEXT("Back to one local."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionCapacitySlotRecyclingTest,
	"UnrealSplit.Session.Capacity.SlotRecycling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionCapacitySlotRecyclingTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return false;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	UGameInstance* const GameInstance = Sessions->GetGameInstance();
	if (!Sessions->AddLocalCoopPlayer())
	{
		AddWarning(TEXT("Could not add second local for slot recycling."));
		return true;
	}

	TestEqual(TEXT("Filled to two."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 2);
	TestTrue(TEXT("Unregister one (secondary)."), Sessions->RemoveLocalCoopPlayer());
	TestEqual(TEXT("One seat free."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 1);
	TestTrue(TEXT("Re-register into freed seat."), Sessions->AddLocalCoopPlayer());
	TestEqual(TEXT("Back to two after recycle."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 2);
	AssertInvariants(*this, Sessions, TEXT("SlotRecycling"));

	Sessions->RemoveLocalCoopPlayer();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionCapacityHostClampDegenerateTest,
	"UnrealSplit.Session.Capacity.HostClampDegenerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionCapacityHostClampDegenerateTest::RunTest(const FString& Parameters)
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

	// Host 0 → clamp to 1; Host 99 → clamp to MaxTotalPlayers (2).
	BindHostResult(Context);
	ArmDeadline(Context, 30.0);
	Sessions->HostSession(0);

	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));

	struct FAssertClamp : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		int32 ExpectedPublic = 0;
		explicit FAssertClamp(FContextPtr InCtx, int32 InExpected) : Ctx(MoveTemp(InCtx)), ExpectedPublic(InExpected) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}
			FAutomationTestBase& Test = *Ctx->Test;
			const int32 Public = GetOssNumPublicConnections(Ctx->Sessions.Get());
			Test.TestEqual(TEXT("Advertised public connections after clamp."), Public, ExpectedPublic);
			Test.TestTrue(TEXT("Session exists after degenerate Host."), HasNamedGameSession(Ctx->Sessions.Get()));
			AssertInvariants(Test, Ctx->Sessions.Get(), TEXT("HostClamp"));
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FAssertClamp(Context, 1));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));

	struct FHost99 : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FHost99(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || !Ctx->Sessions.IsValid())
			{
				return true;
			}
			BindHostResult(Ctx);
			ArmDeadline(Ctx, 30.0);
			Ctx->Sessions->HostSession(99);
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FHost99(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FAssertClamp(Context, 2));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
