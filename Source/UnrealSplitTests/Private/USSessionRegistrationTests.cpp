// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/LocalPlayer.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Misc/AutomationTest.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"

#include "USSessionSubsystem.h"
#include "USSessionTestHelpers.h"
#include "USSessionTypes.h"
#include "USShadowLocalPlayer.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Group 4 — registration idempotence & local player removal (Null, single process).
 * Subsystem register path is private; double-register is exercised via OSS after Host.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionRegistrationPrimaryRemovalRefusedTest,
	"UnrealSplit.Session.Registration.PrimaryRemovalRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionRegistrationPrimaryRemovalRefusedTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	TestFalse(TEXT("Primary RemoveLocalCoopPlayer refused."), Sessions->RemoveLocalCoopPlayer());
	TestEqual(TEXT("Still one local."), UUSShadowLocalPlayer::CountRealLocalPlayers(Sessions->GetGameInstance()), 1);
	AssertInvariants(*this, Sessions, TEXT("PrimaryRemoval"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionRegistrationSecondaryRemovalTest,
	"UnrealSplit.Session.Registration.SecondaryRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionRegistrationSecondaryRemovalTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	UGameInstance* const GameInstance = Sessions->GetGameInstance();
	if (!Sessions->AddLocalCoopPlayer())
	{
		AddWarning(TEXT("Could not add secondary local."));
		return true;
	}

	TestEqual(TEXT("Two locals before remove."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 2);
	TestTrue(TEXT("Secondary remove succeeds."), Sessions->RemoveLocalCoopPlayer());
	TestEqual(TEXT("One local after secondary remove."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 1);
	AssertInvariants(*this, Sessions, TEXT("SecondaryRemoval"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionRegistrationDoubleUnregisterHarmlessTest,
	"UnrealSplit.Session.Registration.DoubleUnregisterHarmless",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionRegistrationDoubleUnregisterHarmlessTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	UGameInstance* const GameInstance = Sessions->GetGameInstance();
	if (!Sessions->AddLocalCoopPlayer())
	{
		AddWarning(TEXT("Could not add secondary for double-unregister."));
		return true;
	}

	TestTrue(TEXT("First secondary remove."), Sessions->RemoveLocalCoopPlayer());
	TestFalse(TEXT("Second remove is primary-guard refusal (harmless)."), Sessions->RemoveLocalCoopPlayer());
	TestEqual(TEXT("Still exactly one local."), UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance), 1);
	AssertInvariants(*this, Sessions, TEXT("DoubleUnregister"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionRegistrationDoubleRegisterNoDuplicateTest,
	"UnrealSplit.Session.Registration.DoubleRegisterNoDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionRegistrationDoubleRegisterNoDuplicateTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	BindHostResult(Context);
	ArmDeadline(Context, 30.0);
	Sessions->HostSession(2);

	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));

	struct FDoubleRegister : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FDoubleRegister(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}

			FAutomationTestBase& Test = *Ctx->Test;
			UUSSessionSubsystem* const SessionsInner = Ctx->Sessions.Get();
			const int32 Before = GetOssRegisteredPlayerCount(SessionsInner);
			Test.TestTrue(TEXT("Host should have registered at least the primary."), Before >= 1);

			ULocalPlayer* const Local = UUSShadowLocalPlayer::GetRealLocalPlayerByIndex(SessionsInner->GetGameInstance(), 0);
			const FUniqueNetIdRepl Id = Local != nullptr ? Local->GetPreferredUniqueNetId() : FUniqueNetIdRepl();
			if (!Id.IsValid())
			{
				Test.AddWarning(TEXT("Primary local has no UniqueNetId; skip OSS double-register."));
				return true;
			}

			IOnlineSessionPtr Oss = GetOssSessions(SessionsInner);
			if (!Oss.IsValid())
			{
				Test.AddError(TEXT("No OSS session interface."));
				return true;
			}

			Oss->RegisterPlayer(NAME_GameSession, *Id, false);
			const int32 After = GetOssRegisteredPlayerCount(SessionsInner);
			Test.TestEqual(TEXT("Second RegisterPlayer must not duplicate the same id."), After, Before);
			AssertInvariants(Test, SessionsInner, TEXT("DoubleRegister"));
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FDoubleRegister(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSSessionRegistrationUnregisterNeverRegisteredTest,
	"UnrealSplit.Session.Registration.UnregisterNeverRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUSSessionRegistrationUnregisterNeverRegisteredTest::RunTest(const FString& Parameters)
{
	using namespace USSessionTest;

	FContextPtr Context = MakeShared<FContext, ESPMode::ThreadSafe>();
	UUSSessionSubsystem* const Sessions = ResolveSubsystem(*this, Context);
	if (Sessions == nullptr)
	{
		return true;
	}

	if (!EnsureSingleLocalPlayer(*this, Sessions))
	{
		return false;
	}

	if (Sessions->GetSessionState() != EUSSessionState::Idle || HasNamedGameSession(Sessions))
	{
		RequestLeave(Sessions);
	}

	BindHostResult(Context);
	ArmDeadline(Context, 30.0);
	Sessions->HostSession(2);

	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitHosting(Context));

	struct FUnregisterStranger : public IAutomationLatentCommand
	{
		FContextPtr Ctx;
		explicit FUnregisterStranger(FContextPtr InCtx) : Ctx(MoveTemp(InCtx)) {}
		virtual bool Update() override
		{
			if (!Ctx.IsValid() || Ctx->Test == nullptr || !Ctx->Sessions.IsValid())
			{
				return true;
			}

			FAutomationTestBase& Test = *Ctx->Test;
			UUSSessionSubsystem* const SessionsInner = Ctx->Sessions.Get();
			const int32 Before = GetOssRegisteredPlayerCount(SessionsInner);

			IOnlineSessionPtr Oss = GetOssSessions(SessionsInner);
			IOnlineSubsystem* const OssSys = IOnlineSubsystem::Get();
			IOnlineIdentityPtr Identity = OssSys != nullptr ? OssSys->GetIdentityInterface() : nullptr;
			if (!Oss.IsValid() || !Identity.IsValid())
			{
				Test.AddWarning(TEXT("OSS/identity unavailable for never-registered unregister."));
				return true;
			}

			// A fabricated Null id that was never registered.
			const FUniqueNetIdPtr Stranger = Identity->CreateUniquePlayerId(TEXT("UnrealSplit-NeverRegistered-TestId"));
			if (!Stranger.IsValid())
			{
				Test.AddWarning(TEXT("Could not mint a stranger UniqueNetId."));
				return true;
			}

			Oss->UnregisterPlayer(NAME_GameSession, *Stranger);
			Test.TestEqual(TEXT("Unregister never-registered must not change RegisteredPlayers count."),
				GetOssRegisteredPlayerCount(SessionsInner), Before);
			AssertInvariants(Test, SessionsInner, TEXT("UnregisterNeverRegistered"));
			return true;
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FUnregisterStranger(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentLeave(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentWaitIdle(Context));
	ADD_LATENT_AUTOMATION_COMMAND(FUSLatentAssertIdleInvariants(Context));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
