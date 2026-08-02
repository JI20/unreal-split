// Copyright (c) Jonas Itt. All Rights Reserved.

#include "CoreMinimal.h"

#include "Camera/CameraActor.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Class.h"

#include "USConsoleReport.h"
#include "USViewState.h"
#include "USViewSubsystem.h"

// Development-only driver for the view layer until level volumes and puzzle
// logic call it. The console is compiled out of shipping builds, so these go
// with it.
#if !UE_BUILD_SHIPPING

using USConsole::Report;
using USConsole::ReportWarning;

namespace
{
	float ParseShare(const TArray<FString>& Args, int32 ArgIndex, float DefaultShare)
	{
		return Args.Num() > ArgIndex ? FCString::Atof(*Args[ArgIndex]) : DefaultShare;
	}

	float ParseBlend(const TArray<FString>& Args, int32 ArgIndex, float DefaultBlend)
	{
		return Args.Num() > ArgIndex ? FCString::Atof(*Args[ArgIndex]) : DefaultBlend;
	}

	/**
	 * Answers the authority question before the command runs.
	 *
	 * A refused layout change and a layout change that landed both leave the
	 * screen exactly as it was on a machine with one view, so saying which of the
	 * two happened is the whole value of these commands on a client.
	 */
	bool ReportRefusedOnClient(const UUSViewSubsystem& View, FOutputDevice& Ar, const TCHAR* CommandName)
	{
		if (View.IsViewAuthority())
		{
			return false;
		}

		ReportWarning(Ar, FString::Printf(TEXT("%s: refused. This machine is a client and the screen layout is server-authoritative. Run it on the host, or use US.SplitRatio on the host to move both machines at once."), CommandName));
		return true;
	}

	/**
	 * Names the optional arguments in the answer whenever a default was used.
	 *
	 * Console help is only visible to whoever thinks to ask for it, so a command
	 * that quietly picked 0.5 and one second looks like a command with no choice
	 * in the matter.
	 */
	FString DescribeDefaults(int32 ArgsGiven, int32 ArgsAccepted, const TCHAR* Usage)
	{
		if (ArgsGiven >= ArgsAccepted)
		{
			return FString();
		}

		// The trailing argument of both ratio commands is the blend, so the only
		// two cases are "the blend was chosen for you" and "everything was".
		return FString::Printf(TEXT(" (%s defaulted — %s)"),
			ArgsGiven == 0 ? TEXT("everything") : TEXT("blend time"),
			Usage);
	}

	/**
	 * Whether a remote machine can resolve a reference to this actor.
	 *
	 * Two separate routes, and a level-placed camera takes the second one: a
	 * replicated actor is addressable because replication assigns it a net id,
	 * while an actor loaded from the level is addressable by path name and needs
	 * no replication at all. Only an actor that is neither — something spawned
	 * locally at runtime — is invisible to the other machine.
	 */
	const TCHAR* DescribeRemoteAddressability(const AActor& ViewTarget)
	{
		if (ViewTarget.IsNameStableForNetworking())
		{
			return TEXT("placed in the level, so remote machines resolve it by path");
		}

		if (ViewTarget.GetIsReplicated())
		{
			return TEXT("replicated, so remote machines resolve it");
		}

		return TEXT("WARNING: neither replicated nor level-placed, so remote machines cannot resolve it and will keep their own cameras");
	}

	/**
	 * Everything in this world worth naming to US.Merge: every camera actor the
	 * level holds, plus whatever each local player is currently looking through.
	 *
	 * Deliberately not every actor, even though US.Merge accepts any of them —
	 * a list that includes the floor is not a list anyone reads. The current
	 * view targets earn their place by being the default the command already
	 * uses, and they are usually pawns rather than camera actors.
	 */
	TArray<AActor*> GatherMergeCandidates(UWorld* World)
	{
		TArray<AActor*> Candidates;

		for (TActorIterator<ACameraActor> It(World); It; ++It)
		{
			Candidates.Add(*It);
		}

		for (ULocalPlayer* const LocalPlayer : GEngine->GetGamePlayers(World))
		{
			if (LocalPlayer == nullptr || LocalPlayer->PlayerController == nullptr)
			{
				continue;
			}

			if (AActor* const ViewTarget = LocalPlayer->PlayerController->GetViewTarget())
			{
				Candidates.AddUnique(ViewTarget);
			}
		}

		return Candidates;
	}

	/**
	 * Resolves one actor by name or editor label for US.Merge.
	 *
	 * Refuses rather than falling back to player 1's camera: a typo that merged
	 * onto something reasonable would look like the named actor working.
	 */
	AActor* FindViewTargetByName(UWorld* World, const FString& Name, FOutputDevice& Ar)
	{
		TArray<AActor*> ExactMatches;
		TArray<AActor*> PartialMatches;
		TArray<FString> CameraActorNames;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* const Actor = *It;
			const FString ActorName = Actor->GetName();
			const FString ActorLabel = Actor->GetActorNameOrLabel();

			if (ActorName.Equals(Name, ESearchCase::IgnoreCase) || ActorLabel.Equals(Name, ESearchCase::IgnoreCase))
			{
				ExactMatches.Add(Actor);
			}
			else if (ActorName.Contains(Name) || ActorLabel.Contains(Name))
			{
				PartialMatches.Add(Actor);
			}

			if (Actor->IsA<ACameraActor>())
			{
				CameraActorNames.Add(ActorLabel);
			}
		}

		// A partial match is what makes this usable by hand: level actors carry
		// generated suffixes, so "SideCam" should find "SideCam_2" without anyone
		// having to read the outliner first.
		const TArray<AActor*>& Matches = ExactMatches.Num() > 0 ? ExactMatches : PartialMatches;

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}

		if (Matches.Num() == 0)
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.Merge: nothing in this world is called '%s', so nothing merged. Camera actors here: %s. Run US.ViewTargets for the full list of names."),
				*Name,
				CameraActorNames.Num() > 0 ? *FString::Join(CameraActorNames, TEXT(", ")) : TEXT("none")));
			return nullptr;
		}

		TArray<FString> MatchNames;
		for (const AActor* const Match : Matches)
		{
			MatchNames.Add(Match->GetActorNameOrLabel());
		}

		ReportWarning(Ar, FString::Printf(TEXT("US.Merge: '%s' matches %d actors (%s), so nothing merged. Name one of them exactly."),
			*Name, Matches.Num(), *FString::Join(MatchNames, TEXT(", "))));
		return nullptr;
	}
}

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSSplitRatioCommand(
	TEXT("US.SplitRatio"),
	TEXT("UnrealSplit: set the share of the screen width given to player 1. Usage: US.SplitRatio <share 0..1> [blend seconds] (blend defaults to 1). Server only; does not leave a merged view."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSViewSubsystem* View = USConsole::FindSubsystem<UUSViewSubsystem>(World, Ar);
		if (View == nullptr)
		{
			return;
		}

		if (Args.Num() < 1)
		{
			ReportWarning(Ar, TEXT("US.SplitRatio: needs a share. Usage: US.SplitRatio <share 0..1> [blend seconds]."));
			return;
		}

		if (ReportRefusedOnClient(*View, Ar, TEXT("US.SplitRatio")))
		{
			return;
		}

		const float Share = ParseShare(Args, 0, 0.5f);
		const float BlendTime = ParseBlend(Args, 1, 1.0f);

		Report(Ar, FString::Printf(TEXT("US.SplitRatio: player 1 gets %.3f of the width over %.2fs.%s"),
			FMath::Clamp(Share, 0.0f, 1.0f),
			BlendTime,
			*DescribeDefaults(Args.Num(), 2, TEXT("US.SplitRatio <share> <blend>"))));
		View->SetSplitRatio(Share, BlendTime);
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSSplitCommand(
	TEXT("US.Split"),
	TEXT("UnrealSplit: return both players to their own views. Usage: US.Split [share 0..1] [blend seconds] (defaults to 0.5 over 1s). Server only."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSViewSubsystem* View = USConsole::FindSubsystem<UUSViewSubsystem>(World, Ar);
		if (View == nullptr)
		{
			return;
		}

		if (ReportRefusedOnClient(*View, Ar, TEXT("US.Split")))
		{
			return;
		}

		const float Share = ParseShare(Args, 0, 0.5f);
		const float BlendTime = ParseBlend(Args, 1, 1.0f);

		Report(Ar, FString::Printf(TEXT("US.Split: splitting at %.3f over %.2fs.%s"),
			FMath::Clamp(Share, 0.0f, 1.0f),
			BlendTime,
			*DescribeDefaults(Args.Num(), 2, TEXT("US.Split <share> <blend>"))));
		View->SplitViews(Share, BlendTime);
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSMergeCommand(
	TEXT("US.Merge"),
	TEXT("UnrealSplit: collapse both players onto one full-screen view. Usage: US.Merge [blend seconds] [actor name]; either argument may be omitted. Server only. Blend defaults to 1 second and the view target defaults to player 1's own camera; name an actor (a level camera, for instance) to merge onto that instead, and run US.ViewTargets for the names to type."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSViewSubsystem* View = USConsole::FindSubsystem<UUSViewSubsystem>(World, Ar);
		if (View == nullptr)
		{
			return;
		}

		if (ReportRefusedOnClient(*View, Ar, TEXT("US.Merge")))
		{
			return;
		}

		// Either argument may be omitted and the order does not matter, because the
		// blend is simply the numeric one: US.Merge, US.Merge 2, US.Merge SideCam,
		// US.Merge 2 SideCam and US.Merge SideCam 2 all mean what they look like.
		float BlendTime = 1.0f;
		bool bBlendGiven = false;
		int32 NameArgIndex = INDEX_NONE;

		for (int32 ArgIndex = 0; ArgIndex < Args.Num(); ++ArgIndex)
		{
			if (!bBlendGiven && Args[ArgIndex].IsNumeric())
			{
				BlendTime = FCString::Atof(*Args[ArgIndex]);
				bBlendGiven = true;
			}
			else if (NameArgIndex == INDEX_NONE)
			{
				NameArgIndex = ArgIndex;
			}
		}

		AActor* ViewTarget = nullptr;

		if (NameArgIndex != INDEX_NONE)
		{
			ViewTarget = FindViewTargetByName(World, Args[NameArgIndex], Ar);
			if (ViewTarget == nullptr)
			{
				// FindViewTargetByName already said why, and merging onto something
				// else after a failed lookup would hide the mistake.
				return;
			}
		}
		else
		{
			// Player 1's own camera is the one view target guaranteed to exist
			// without the game having authored anything, which is what makes this
			// testable before any level has a camera to merge onto. It is usually
			// the player's pawn, which replicates, so a client resolves it too.
			APlayerController* const FirstController = GEngine->GetFirstLocalPlayerController(World);
			ViewTarget = FirstController != nullptr ? FirstController->GetViewTarget() : nullptr;
		}

		if (ViewTarget == nullptr)
		{
			ReportWarning(Ar, TEXT("US.Merge: no view target on player 1, so the layout will merge but the cameras will stay put."));
		}
		else
		{
			Report(Ar, FString::Printf(TEXT("US.Merge: merging onto '%s' over %.2fs — %s.%s"),
				*ViewTarget->GetActorNameOrLabel(),
				BlendTime,
				DescribeRemoteAddressability(*ViewTarget),
				bBlendGiven ? TEXT("") : TEXT(" (blend time defaulted — US.Merge <blend> [actor])")));
		}

		View->MergeViews(BlendTime, ViewTarget);
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSViewTargetsCommand(
	TEXT("US.ViewTargets"),
	TEXT("UnrealSplit: list what US.Merge can merge onto — every camera actor in this level plus each local player's current view target — with the exact name to type and whether a remote machine can resolve it. Usage: US.ViewTargets [name filter]."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (World == nullptr)
		{
			ReportWarning(Ar, TEXT("US.ViewTargets: no world. Run this from a running game or PIE session."));
			return;
		}

		const FString Filter = Args.Num() > 0 ? Args[0] : FString();
		const FString FilterNote = Filter.IsEmpty() ? FString() : FString::Printf(TEXT(" matching '%s'"), *Filter);

		// The one candidate a bare US.Merge would pick, worth marking so the list
		// also answers "what am I looking at right now".
		const APlayerController* const FirstController = GEngine->GetFirstLocalPlayerController(World);
		const AActor* const DefaultTarget = FirstController != nullptr ? FirstController->GetViewTarget() : nullptr;

		TArray<FString> Lines;

		for (AActor* const Candidate : GatherMergeCandidates(World))
		{
			const FString Label = Candidate->GetActorNameOrLabel();
			const FString Name = Candidate->GetName();

			if (!Filter.IsEmpty() && !Label.Contains(Filter) && !Name.Contains(Filter))
			{
				continue;
			}

			// Both spellings, because the label US.Merge matches first and the
			// outliner shows is editor-only: the same actor answers to its object
			// name in a packaged build, where the label is gone.
			const FString Names = Label.Equals(Name)
				? Label
				: FString::Printf(TEXT("%s (or %s)"), *Label, *Name);

			const TCHAR* const DefaultNote = Candidate == DefaultTarget
				? TEXT(" Player 1 is looking through this one now, so a bare US.Merge picks it.")
				: TEXT("");

			Lines.Add(FString::Printf(TEXT("  %s [%s] — %s.%s"),
				*Names,
				*Candidate->GetClass()->GetName(),
				DescribeRemoteAddressability(*Candidate),
				DefaultNote));
		}

		if (Lines.Num() == 0)
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.ViewTargets: nothing to list in '%s'%s. US.Merge takes any actor, so this only means the level holds no camera actors and no local player has a view target yet — place a CameraActor to get a fixed camera to merge onto."),
				*World->GetName(), *FilterNote));
			return;
		}

		Report(Ar, FString::Printf(TEXT("US.ViewTargets: %d candidate(s) in '%s'%s. Type a name exactly as listed: US.Merge [blend seconds] <name>."),
			Lines.Num(), *World->GetName(), *FilterNote));

		for (const FString& Line : Lines)
		{
			Report(Ar, Line);
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSViewStateCommand(
	TEXT("US.ViewState"),
	TEXT("UnrealSplit: print the current screen layout — live ratio, target ratio, merge state, blend progress and whether this machine may change any of it."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		UUSViewSubsystem* View = USConsole::FindSubsystem<UUSViewSubsystem>(World, Ar);
		if (View == nullptr)
		{
			return;
		}

		const FUSViewIntent Intent = View->GetViewIntent();

		Report(Ar, FString::Printf(TEXT("View state: Player1Share=%.3f -> %.3f (blend %.0f%%%s), Merged=%s, Authority=%s"),
			View->GetCurrentSplitRatio(),
			View->GetTargetSplitRatio(),
			View->GetBlendProgress() * 100.0f,
			View->IsLocalSplitRatioDriveActive() ? TEXT(", local drive holding the replicated intent off") : TEXT(""),
			Intent.bMerged ? TEXT("yes") : TEXT("no"),
			View->IsViewAuthority() ? TEXT("yes, layout changes allowed here") : TEXT("no, this is a client")));

		Report(Ar, FString::Printf(TEXT("View intent: Player1Share=%.3f BlendTime=%.2fs Easing=%s MergedViewTarget=%s"),
			Intent.Player1Share,
			Intent.BlendTime,
			*UEnum::GetValueAsString(Intent.Easing),
			*GetNameSafe(Intent.MergedViewTarget.Get())));

		// Two separate facts, and both are worth seeing: no actor at all means the
		// authority never spawned one, while an actor whose intent differs from the
		// one above means replication arrived and the local layout has not caught
		// up yet — or that a local drive is deliberately ignoring it.
		const AUSViewState* const ViewState = View->GetViewState();
		if (ViewState == nullptr)
		{
			ReportWarning(Ar, TEXT("View state actor: none in this world. On a client it has not replicated yet; on the host that means nothing has spawned it."));
		}
		else
		{
			const FUSViewIntent Replicated = ViewState->GetViewIntent();
			Report(Ar, FString::Printf(TEXT("View state actor: '%s', replicated intent Player1Share=%.3f Merged=%s"),
				*ViewState->GetName(),
				Replicated.Player1Share,
				Replicated.bMerged ? TEXT("yes") : TEXT("no")));
		}

		Report(Ar, FString::Printf(TEXT("Local views: %d. One view is laid out full screen; the split needs two."),
			GEngine->GetNumGamePlayers(World)));
	}));

#endif // !UE_BUILD_SHIPPING
