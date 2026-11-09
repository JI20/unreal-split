// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/StringOutputDevice.h"

#include "USGameViewportClient.h"
#include "USViewTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Audio listener policy — state logic only.
 *
 * What a listener actually hears is not asserted anywhere here: it would mean
 * reading a rendered mix, and the whole point of the policy is that the engine
 * mixes for us. What is asserted is everything that decides where listeners go:
 * how Auto resolves, what the command accepts, and that repeating a switch or
 * feeding it nonsense leaves the machine in the state it was already in.
 */

namespace USAudioPolicyTest
{
	/** The viewport client of a live PIE/game world, or null (with a skip warning). */
	static UUSGameViewportClient* ResolveViewport(FAutomationTestBase& Test, UWorld*& OutWorld)
	{
		OutWorld = nullptr;

		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType != EWorldType::PIE && WorldContext.WorldType != EWorldType::Game)
			{
				continue;
			}

			UWorld* const World = WorldContext.World();
			UGameInstance* const GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
			if (GameInstance == nullptr)
			{
				continue;
			}

			if (UUSGameViewportClient* const Viewport = Cast<UUSGameViewportClient>(GameInstance->GetGameViewportClient()))
			{
				OutWorld = World;
				return Viewport;
			}
		}

		Test.AddWarning(TEXT("Skipped: no PIE/game world with a UUSGameViewportClient. Start Play In Editor or a standalone game first."));
		return nullptr;
	}

	/**
	 * Puts the machine's policy back exactly as the test found it.
	 *
	 * This is a live preference of whoever is playing, and the suite is expected to
	 * be run against a session somebody is sitting in front of.
	 */
	struct FScopedPolicyRestore
	{
		TWeakObjectPtr<UUSGameViewportClient> Viewport;
		bool bWasOverridden = false;
		EUSAudioListenerMode PreviousMode = EUSAudioListenerMode::Own;

		explicit FScopedPolicyRestore(UUSGameViewportClient* InViewport)
			: Viewport(InViewport)
			, bWasOverridden(InViewport != nullptr && InViewport->IsAudioListenerModeOverridden())
			, PreviousMode(InViewport != nullptr ? InViewport->GetAudioListenerMode() : EUSAudioListenerMode::Own)
		{
		}

		~FScopedPolicyRestore()
		{
			if (UUSGameViewportClient* const Client = Viewport.Get())
			{
				if (bWasOverridden)
				{
					Client->SetAudioListenerMode(PreviousMode);
				}
				else
				{
					Client->ResetAudioListenerModeToConfig();
				}
			}
		}
	};

	static FString Describe(EUSAudioListenerMode Mode)
	{
		return Mode == EUSAudioListenerMode::Shared ? TEXT("shared") : TEXT("own");
	}

	/**
	 * Compared as strings on purpose: FAutomationTestBase::TestEqual has nothing to
	 * print for a bare enum, so a failure would read "the two values are not equal"
	 * and leave you to guess which way round it went.
	 */
	static void TestMode(FAutomationTestBase& Test, const TCHAR* What, EUSAudioListenerMode Actual, EUSAudioListenerMode Expected)
	{
		Test.TestEqual(What, Describe(Actual), Describe(Expected));
	}

	static bool RunCommand(UWorld* World, const TCHAR* Command, FString& OutOutput)
	{
		FStringOutputDevice Output;
		const bool bRecognised = IConsoleManager::Get().ProcessUserConsoleInput(Command, Output, World);
		OutOutput = Output;
		return bRecognised;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSAudioPolicyAutoResolvesPerTopologyTest,
	"UnrealSplit.Audio.Policy.AutoResolvesPerTopology",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSAudioPolicyAutoResolvesPerTopologyTest::RunTest(const FString& Parameters)
{
	using namespace USAudioPolicyTest;

	// The default must not change what any existing session sounds like: couch had
	// two listeners and keeps them, a machine showing a shadow view had one.
	TestMode(*this, TEXT("Auto without a shadow view is shared (couch keeps both ears)."),
		UUSGameViewportClient::ResolveAutoAudioListenerMode(false), EUSAudioListenerMode::Shared);

	TestMode(*this, TEXT("Auto with a shadow view is own (a partner's camera lends no ears)."),
		UUSGameViewportClient::ResolveAutoAudioListenerMode(true), EUSAudioListenerMode::Own);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSAudioPolicyCommandSetsModeTest,
	"UnrealSplit.Audio.Policy.CommandSetsMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSAudioPolicyCommandSetsModeTest::RunTest(const FString& Parameters)
{
	using namespace USAudioPolicyTest;

	UWorld* World = nullptr;
	UUSGameViewportClient* const Viewport = ResolveViewport(*this, World);
	if (Viewport == nullptr)
	{
		return true;
	}

	FScopedPolicyRestore Restore(Viewport);

	FString Output;
	TestTrue(TEXT("US.AudioMode own is a recognised command."), RunCommand(World, TEXT("US.AudioMode own"), Output));
	TestMode(*this, TEXT("Own after the command."), Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Own);
	TestTrue(TEXT("Command counts as an override of the configured default."), Viewport->IsAudioListenerModeOverridden());

	TestTrue(TEXT("US.AudioMode shared is a recognised command."), RunCommand(World, TEXT("US.AudioMode shared"), Output));
	TestMode(*this, TEXT("Shared after the command."), Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Shared);

	// No argument reports rather than changes anything.
	TestTrue(TEXT("US.AudioMode with no argument is recognised."), RunCommand(World, TEXT("US.AudioMode"), Output));
	TestMode(*this, TEXT("Reporting does not change the mode."), Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Shared);
	TestTrue(TEXT("Reporting says which mode is in force."), Output.Contains(TEXT("shared")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSAudioPolicySwitchingIsIdempotentTest,
	"UnrealSplit.Audio.Policy.SwitchingIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSAudioPolicySwitchingIsIdempotentTest::RunTest(const FString& Parameters)
{
	using namespace USAudioPolicyTest;

	UWorld* World = nullptr;
	UUSGameViewportClient* const Viewport = ResolveViewport(*this, World);
	if (Viewport == nullptr)
	{
		return true;
	}

	FScopedPolicyRestore Restore(Viewport);

	FString Output;
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		RunCommand(World, TEXT("US.AudioMode shared"), Output);
		TestMode(*this, TEXT("Shared stays shared however often it is asked for."),
			Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Shared);
	}

	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		Viewport->SetAudioListenerMode(EUSAudioListenerMode::Own);
		TestMode(*this, TEXT("Own stays own however often it is set."),
			Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Own);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSAudioPolicyBadArgumentKeepsModeTest,
	"UnrealSplit.Audio.Policy.BadArgumentKeepsMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSAudioPolicyBadArgumentKeepsModeTest::RunTest(const FString& Parameters)
{
	using namespace USAudioPolicyTest;

	UWorld* World = nullptr;
	UUSGameViewportClient* const Viewport = ResolveViewport(*this, World);
	if (Viewport == nullptr)
	{
		return true;
	}

	FScopedPolicyRestore Restore(Viewport);

	FString Output;
	RunCommand(World, TEXT("US.AudioMode own"), Output);

	// Refusing is the expected outcome here, so the warnings it logs are expected too
	// — "auto" included: that is a statement about defaults, not a way to hear the
	// game, so the runtime command treats it like any other unknown word.
	AddExpectedMessagePlain(TEXT("is neither own nor shared"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	RunCommand(World, TEXT("US.AudioMode auto"), Output);
	TestMode(*this, TEXT("An unknown argument leaves the mode alone."), Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Own);

	RunCommand(World, TEXT("US.AudioMode sideways"), Output);
	TestMode(*this, TEXT("Still own after a second unknown argument."), Viewport->GetAudioListenerMode(), EUSAudioListenerMode::Own);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSAudioPolicyResetReturnsToConfigTest,
	"UnrealSplit.Audio.Policy.ResetReturnsToConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSAudioPolicyResetReturnsToConfigTest::RunTest(const FString& Parameters)
{
	using namespace USAudioPolicyTest;

	UWorld* World = nullptr;
	UUSGameViewportClient* const Viewport = ResolveViewport(*this, World);
	if (Viewport == nullptr)
	{
		return true;
	}

	FScopedPolicyRestore Restore(Viewport);

	Viewport->ResetAudioListenerModeToConfig();
	const EUSAudioListenerMode Configured = Viewport->GetAudioListenerMode();

	// Whichever way the configured default resolves, overriding to the other value
	// and dropping the override has to land back on it.
	const EUSAudioListenerMode Other = Configured == EUSAudioListenerMode::Own
		? EUSAudioListenerMode::Shared
		: EUSAudioListenerMode::Own;

	Viewport->SetAudioListenerMode(Other);
	TestMode(*this, TEXT("Override takes effect."), Viewport->GetAudioListenerMode(), Other);
	TestTrue(TEXT("Override is reported as one."), Viewport->IsAudioListenerModeOverridden());

	Viewport->ResetAudioListenerModeToConfig();
	TestFalse(TEXT("Reset drops the override."), Viewport->IsAudioListenerModeOverridden());
	TestMode(*this, TEXT("Reset lands back on the configured default."), Viewport->GetAudioListenerMode(), Configured);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
