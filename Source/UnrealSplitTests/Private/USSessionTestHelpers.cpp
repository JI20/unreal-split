// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USSessionTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"

#include "USSessionSubsystem.h"
#include "USSessionTestListener.h"
#include "USSessionTypes.h"
#include "USShadowLocalPlayer.h"

namespace USSessionTest
{
	void FContext::ClearDelegates()
	{
		if (Listener.IsValid())
		{
			Listener->UnbindAll(Sessions.Get());
		}
	}

	UUSSessionTestListener* FContext::EnsureListener()
	{
		if (!Listener.IsValid())
		{
			Listener.Reset(NewObject<UUSSessionTestListener>(GetTransientPackage()));
		}
		return Listener.Get();
	}

	bool FContext::HostCallbackSeen() const
	{
		return Listener.IsValid() && Listener->bHostSeen;
	}

	bool FContext::HostSucceeded() const
	{
		return Listener.IsValid() && Listener->bHostOk;
	}

	bool FContext::JoinCallbackSeen() const
	{
		return Listener.IsValid() && Listener->bJoinSeen;
	}

	bool FContext::JoinSucceeded() const
	{
		return Listener.IsValid() && Listener->bJoinOk;
	}

	UUSSessionSubsystem* ResolveSubsystem(FAutomationTestBase& Test, FContextPtr Context)
	{
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

			if (UUSSessionSubsystem* Sessions = GameInstance->GetSubsystem<UUSSessionSubsystem>())
			{
				if (Context.IsValid())
				{
					Context->Sessions = Sessions;
					Context->Test = &Test;
				}
				return Sessions;
			}
		}

		Test.AddWarning(TEXT("Skipped: no PIE/game world. Start Play In Editor, then run UnrealSplit.Session tests."));
		return nullptr;
	}

	IOnlineSessionPtr GetOssSessions(const UUSSessionSubsystem* Subsystem)
	{
		const UGameInstance* const GameInstance = Subsystem != nullptr ? Subsystem->GetGameInstance() : nullptr;
		const UWorld* const World = GameInstance != nullptr ? GameInstance->GetWorld() : nullptr;
		return World != nullptr ? Online::GetSessionInterface(World) : nullptr;
	}

	bool HasNamedGameSession(const UUSSessionSubsystem* Subsystem)
	{
		const IOnlineSessionPtr Oss = GetOssSessions(Subsystem);
		return Oss.IsValid() && Oss->GetNamedSession(NAME_GameSession) != nullptr;
	}

	int32 GetOssRegisteredPlayerCount(const UUSSessionSubsystem* Subsystem)
	{
		const IOnlineSessionPtr Oss = GetOssSessions(Subsystem);
		const FNamedOnlineSession* const Session = Oss.IsValid() ? Oss->GetNamedSession(NAME_GameSession) : nullptr;
		return Session != nullptr ? Session->RegisteredPlayers.Num() : 0;
	}

	int32 GetOssNumPublicConnections(const UUSSessionSubsystem* Subsystem)
	{
		const IOnlineSessionPtr Oss = GetOssSessions(Subsystem);
		const FNamedOnlineSession* const Session = Oss.IsValid() ? Oss->GetNamedSession(NAME_GameSession) : nullptr;
		return Session != nullptr ? Session->SessionSettings.NumPublicConnections : INDEX_NONE;
	}

	bool EnsureSingleLocalPlayer(FAutomationTestBase& Test, UUSSessionSubsystem* Sessions)
	{
		if (Sessions == nullptr)
		{
			return false;
		}

		UGameInstance* const GameInstance = Sessions->GetGameInstance();
		int32 Guard = 8;
		while (UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance) > 1 && Guard-- > 0)
		{
			if (!Sessions->RemoveLocalCoopPlayer())
			{
				Test.AddError(TEXT("Could not peel back to a single local player."));
				return false;
			}
		}

		const int32 Count = UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance);
		if (Count != 1)
		{
			Test.AddError(FString::Printf(TEXT("Expected 1 real local player, have %d."), Count));
			return false;
		}
		return true;
	}

	void RequestLeave(UUSSessionSubsystem* Sessions)
	{
		if (Sessions != nullptr)
		{
			Sessions->LeaveSession();
		}
	}

	bool AssertInvariants(FAutomationTestBase& Test, UUSSessionSubsystem* Sessions, const TCHAR* ContextLabel)
	{
		if (Sessions == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("[%s] Sessions subsystem lost."), ContextLabel));
			return false;
		}

		const EUSSessionState State = Sessions->GetSessionState();
		const bool bHasOssSession = HasNamedGameSession(Sessions);
		bool bOk = true;

		if (State == EUSSessionState::Idle && bHasOssSession)
		{
			Test.AddError(FString::Printf(
				TEXT("[%s] Invariant: Idle but OSS still has NAME_GameSession."), ContextLabel));
			bOk = false;
		}

		if ((State == EUSSessionState::Hosting || State == EUSSessionState::InSessionAsClient) && !bHasOssSession)
		{
			Test.AddError(FString::Printf(
				TEXT("[%s] Invariant: state %s but OSS has no NAME_GameSession."),
				ContextLabel, *UEnum::GetValueAsString(State)));
			bOk = false;
		}

		if (bHasOssSession)
		{
			const int32 RealLocals = UUSShadowLocalPlayer::CountRealLocalPlayers(Sessions->GetGameInstance());
			const int32 Registered = GetOssRegisteredPlayerCount(Sessions);
			// Host registers locals; count should match when registration has settled.
			if (State == EUSSessionState::Hosting && Registered > 0 && Registered != RealLocals)
			{
				Test.AddWarning(FString::Printf(
					TEXT("[%s] RegisteredPlayers=%d vs real locals=%d (may still be settling)."),
					ContextLabel, Registered, RealLocals));
			}
		}

		return bOk;
	}

	void BindHostResult(FContextPtr Context)
	{
		if (!Context.IsValid() || !Context->Sessions.IsValid())
		{
			return;
		}

		UUSSessionTestListener* const Listener = Context->EnsureListener();
		Listener->BindHost(Context->Sessions.Get());
	}

	void BindJoinResult(FContextPtr Context)
	{
		if (!Context.IsValid() || !Context->Sessions.IsValid())
		{
			return;
		}

		UUSSessionTestListener* const Listener = Context->EnsureListener();
		Listener->BindJoin(Context->Sessions.Get());
	}

	void ArmDeadline(FContextPtr Context, double SecondsFromNow)
	{
		if (Context.IsValid())
		{
			Context->DeadlineSeconds = FPlatformTime::Seconds() + SecondsFromNow;
		}
	}

	EWaitResult PollUntilHosting(FContextPtr Context)
	{
		if (!Context.IsValid())
		{
			return EWaitResult::LostSubsystem;
		}
		if (!Context->Sessions.IsValid())
		{
			return EWaitResult::LostSubsystem;
		}

		const EUSSessionState State = Context->Sessions->GetSessionState();
		if (State == EUSSessionState::Hosting && HasNamedGameSession(Context->Sessions.Get()))
		{
			return EWaitResult::Success;
		}

		if (Context->HostCallbackSeen() && !Context->HostSucceeded())
		{
			if (Context->Test != nullptr)
			{
				Context->Test->AddError(TEXT("HostSession completed with failure before reaching Hosting."));
			}
			return EWaitResult::Timeout;
		}

		if (FPlatformTime::Seconds() > Context->DeadlineSeconds)
		{
			if (Context->Test != nullptr)
			{
				Context->Test->AddError(FString::Printf(
					TEXT("Timed out waiting for Hosting (state=%s, oss=%d)."),
					*UEnum::GetValueAsString(State),
					HasNamedGameSession(Context->Sessions.Get()) ? 1 : 0));
			}
			return EWaitResult::Timeout;
		}

		return EWaitResult::Pending;
	}

	EWaitResult PollUntilIdleNoSession(FContextPtr Context)
	{
		if (!Context.IsValid())
		{
			return EWaitResult::LostSubsystem;
		}
		if (!Context->Sessions.IsValid())
		{
			return EWaitResult::LostSubsystem;
		}

		const EUSSessionState State = Context->Sessions->GetSessionState();
		if (State == EUSSessionState::Idle && !HasNamedGameSession(Context->Sessions.Get()))
		{
			return EWaitResult::Success;
		}

		if (FPlatformTime::Seconds() > Context->DeadlineSeconds)
		{
			if (Context->Test != nullptr)
			{
				Context->Test->AddError(FString::Printf(
					TEXT("Timed out waiting for Idle with no OSS session (state=%s, oss=%d)."),
					*UEnum::GetValueAsString(State),
					HasNamedGameSession(Context->Sessions.Get()) ? 1 : 0));
			}
			return EWaitResult::Timeout;
		}

		return EWaitResult::Pending;
	}
}

bool FUSLatentWaitHosting::Update()
{
	using namespace USSessionTest;
	const EWaitResult Result = PollUntilHosting(Context);
	return Result != EWaitResult::Pending;
}

bool FUSLatentWaitIdle::Update()
{
	using namespace USSessionTest;
	const EWaitResult Result = PollUntilIdleNoSession(Context);
	return Result != EWaitResult::Pending;
}

bool FUSLatentLeave::Update()
{
	using namespace USSessionTest;
	if (Context.IsValid() && Context->Sessions.IsValid())
	{
		RequestLeave(Context->Sessions.Get());
		ArmDeadline(Context, 30.0);
	}
	return true;
}

bool FUSLatentAssertHostingInvariants::Update()
{
	using namespace USSessionTest;
	if (!Context.IsValid() || Context->Test == nullptr || !Context->Sessions.IsValid())
	{
		return true;
	}

	FAutomationTestBase& Test = *Context->Test;
	UUSSessionSubsystem* const Sessions = Context->Sessions.Get();
	Test.TestEqual(TEXT("State should be Hosting."), Sessions->GetSessionState(), EUSSessionState::Hosting);
	Test.TestTrue(TEXT("OSS NAME_GameSession should exist while hosting."), HasNamedGameSession(Sessions));
	AssertInvariants(Test, Sessions, TEXT("Hosting"));
	Context->ClearDelegates();
	return true;
}

bool FUSLatentAssertIdleInvariants::Update()
{
	using namespace USSessionTest;
	if (!Context.IsValid() || Context->Test == nullptr || !Context->Sessions.IsValid())
	{
		return true;
	}

	FAutomationTestBase& Test = *Context->Test;
	UUSSessionSubsystem* const Sessions = Context->Sessions.Get();
	Test.TestEqual(TEXT("State should be Idle."), Sessions->GetSessionState(), EUSSessionState::Idle);
	Test.TestFalse(TEXT("OSS NAME_GameSession should be gone when Idle."), HasNamedGameSession(Sessions));
	AssertInvariants(Test, Sessions, TEXT("Idle"));
	Context->ClearDelegates();
	return true;
}

bool FUSLatentHostAgain::Update()
{
	using namespace USSessionTest;
	if (!Context.IsValid() || !Context->Sessions.IsValid())
	{
		return true;
	}

	BindHostResult(Context);
	ArmDeadline(Context, 30.0);
	Context->Sessions->HostSession(2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
