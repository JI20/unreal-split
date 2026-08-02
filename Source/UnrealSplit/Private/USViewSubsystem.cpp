// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USViewSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

#include "USGameViewportClient.h"
#include "USViewState.h"
#include "UnrealSplitModule.h"

namespace
{
	// ENetMode is not a reflected enum, so it cannot be printed the way the
	// plugin's own enums are.
	const TCHAR* DescribeNetMode(ENetMode NetMode)
	{
		switch (NetMode)
		{
		case NM_Standalone:
			return TEXT("standalone");

		case NM_ListenServer:
			return TEXT("listen server");

		case NM_DedicatedServer:
			return TEXT("dedicated server");

		case NM_Client:
			return TEXT("client");

		default:
			return TEXT("unknown");
		}
	}
}

void UUSViewSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	WorldChangedHandle = FWorldDelegates::OnGameInstanceWorldChanged.AddUObject(this, &UUSViewSubsystem::HandleGameInstanceWorldChanged);
	ActorsInitializedHandle = FWorldDelegates::OnWorldInitializedActors.AddUObject(this, &UUSViewSubsystem::HandleWorldInitializedActors);

	UE_LOG(LogUnrealSplit, Log, TEXT("View subsystem initialized."));
}

void UUSViewSubsystem::Deinitialize()
{
	FWorldDelegates::OnGameInstanceWorldChanged.Remove(WorldChangedHandle);
	WorldChangedHandle.Reset();

	FWorldDelegates::OnWorldInitializedActors.Remove(ActorsInitializedHandle);
	ActorsInitializedHandle.Reset();

	CachedViewState.Reset();

	UE_LOG(LogUnrealSplit, Log, TEXT("View subsystem deinitialized."));

	Super::Deinitialize();
}

void UUSViewSubsystem::SetSplitRatio(float Player1Share, float BlendTime, EUSViewEasing Easing)
{
	if (!IsViewAuthority())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SetSplitRatio refused: the screen layout is server-authoritative and this machine is a client. Drive the ratio locally with SetSplitRatioLocal, or change it on the server."));
		return;
	}

	AUSViewState* const ViewState = EnsureViewState(GetWorld());
	if (ViewState == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SetSplitRatio ignored: no view state in this world."));
		return;
	}

	ViewState->SetSplitRatio(Player1Share, BlendTime, Easing);
}

void UUSViewSubsystem::SplitViews(float Player1Share, float BlendTime)
{
	if (!IsViewAuthority())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SplitViews refused: the screen layout is server-authoritative and this machine is a client."));
		return;
	}

	AUSViewState* const ViewState = EnsureViewState(GetWorld());
	if (ViewState == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SplitViews ignored: no view state in this world."));
		return;
	}

	ViewState->ClearMerged(Player1Share, BlendTime, EUSViewEasing::EaseInOut);
}

void UUSViewSubsystem::MergeViews(float BlendTime, AActor* ViewTarget)
{
	if (!IsViewAuthority())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("MergeViews refused: the screen layout is server-authoritative and this machine is a client."));
		return;
	}

	AUSViewState* const ViewState = EnsureViewState(GetWorld());
	if (ViewState == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("MergeViews ignored: no view state in this world."));
		return;
	}

	if (ViewTarget == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("MergeViews called without a view target: the layout will merge to one full-screen view but the cameras stay where they are."));
	}
	else if (!ViewTarget->GetIsReplicated() && !ViewTarget->IsNameStableForNetworking())
	{
		// Two routes make a reference resolvable on the other machine, and a
		// level-placed camera takes the second one: replication assigns a net id,
		// while an actor loaded from the level is addressable by its path name and
		// needs no replication at all (AActor::IsNameStableForNetworking, which is
		// what the package map consults). Only an actor spawned locally at runtime
		// is unreachable. Not an error in local co-op, where there is one machine
		// and no reference to send, and not worth refusing over — but it is far
		// cheaper to notice here than on the machine that stayed unmerged.
		UE_LOG(LogUnrealSplit, Warning, TEXT("MergeViews view target '%s' is neither replicated nor placed in the level, so remote machines cannot resolve it and will keep their own cameras. Place it in the level or replicate it — which of the two is the game's call, not the plugin's."),
			*ViewTarget->GetName());
	}

	ViewState->SetMerged(ViewTarget, BlendTime, EUSViewEasing::EaseInOut);
}

void UUSViewSubsystem::SetSplitRatioLocal(float Player1Share)
{
	// Deliberately unguarded: this changes nothing anyone else can see, so a
	// client may drive it, and every machine driving it from the same replicated
	// game state is how a continuously moving ratio stays free.
	if (UUSGameViewportClient* const ViewportClient = GetUSViewportClient())
	{
		ViewportClient->SetSplitRatioLocal(Player1Share);
	}
}

AUSViewState* UUSViewSubsystem::GetViewState() const
{
	return FindViewState(GetWorld());
}

FUSViewIntent UUSViewSubsystem::GetViewIntent() const
{
	// Read from the viewport client rather than from the view state, because this
	// is what the machine is actually applying: on a client that has not received
	// the actor yet the two differ, and the honest answer is the local one.
	const UUSGameViewportClient* const ViewportClient = GetUSViewportClient();
	return ViewportClient != nullptr ? ViewportClient->GetViewIntent() : FUSViewIntent();
}

float UUSViewSubsystem::GetCurrentSplitRatio() const
{
	const UUSGameViewportClient* const ViewportClient = GetUSViewportClient();
	return ViewportClient != nullptr ? ViewportClient->GetCurrentSplitRatio() : 0.5f;
}

float UUSViewSubsystem::GetTargetSplitRatio() const
{
	const UUSGameViewportClient* const ViewportClient = GetUSViewportClient();
	return ViewportClient != nullptr ? ViewportClient->GetTargetSplitRatio() : 0.5f;
}

float UUSViewSubsystem::GetBlendProgress() const
{
	const UUSGameViewportClient* const ViewportClient = GetUSViewportClient();
	return ViewportClient != nullptr ? ViewportClient->GetBlendProgress() : 1.0f;
}

bool UUSViewSubsystem::IsLocalSplitRatioDriveActive() const
{
	const UUSGameViewportClient* const ViewportClient = GetUSViewportClient();
	return ViewportClient != nullptr && ViewportClient->IsLocalSplitRatioDriveActive();
}

bool UUSViewSubsystem::IsViewAuthority() const
{
	const UWorld* const World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_Client;
}

void UUSViewSubsystem::ApplyViewIntent(const FUSViewIntent& Intent)
{
	if (UUSGameViewportClient* const ViewportClient = GetUSViewportClient())
	{
		ViewportClient->SetViewIntent(Intent);
		return;
	}

	const UGameInstance* const GameInstance = GetGameInstance();
	if (GameInstance != nullptr && GameInstance->GetGameViewportClient() != nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("View intent arrived but this game's viewport client is not a UUSGameViewportClient, so the layout will stay on the engine's fixed presets. Check GameViewportClientClassName in DefaultEngine.ini."));
	}
	else
	{
		// No viewport at all: a dedicated server or a commandlet. The intent still
		// replicated, there is simply no screen here to divide.
		UE_LOG(LogUnrealSplit, Verbose, TEXT("View intent applied on a process with no viewport."));
	}
}

AUSViewState* UUSViewSubsystem::EnsureViewState(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	if (AUSViewState* const Existing = FindViewState(World))
	{
		return Existing;
	}

	// A client's copy arrives by replication and must never be spawned locally:
	// a locally spawned one would take the replicated one's place in every lookup
	// and then sit there at its defaults, ignoring the server.
	if (World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	AUSViewState* const Spawned = World->SpawnActor<AUSViewState>();
	CachedViewState = Spawned;

	if (Spawned != nullptr)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Spawned the view state in world '%s' (%s)."),
			*World->GetName(), DescribeNetMode(World->GetNetMode()));

		// Puts this world's intent onto this machine's screen. On the authority
		// nothing else would: the client path runs from the actor's BeginPlay.
		ApplyViewIntent(Spawned->GetViewIntent());
	}
	else
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("Failed to spawn the view state in world '%s'; layout changes will be ignored."), *World->GetName());
	}

	return Spawned;
}

AUSViewState* UUSViewSubsystem::FindViewState(UWorld* World) const
{
	if (World == nullptr)
	{
		return nullptr;
	}

	// The cache is checked against the world it came from, because travel leaves
	// a perfectly valid actor belonging to a world nobody is playing any more.
	if (AUSViewState* const Cached = CachedViewState.Get())
	{
		if (Cached->GetWorld() == World)
		{
			return Cached;
		}
	}

	for (TActorIterator<AUSViewState> It(World); It; ++It)
	{
		CachedViewState = *It;
		return *It;
	}

	CachedViewState.Reset();
	return nullptr;
}

UUSGameViewportClient* UUSViewSubsystem::GetUSViewportClient() const
{
	const UGameInstance* const GameInstance = GetGameInstance();
	return GameInstance != nullptr ? Cast<UUSGameViewportClient>(GameInstance->GetGameViewportClient()) : nullptr;
}

void UUSViewSubsystem::HandleWorldInitializedActors(const FActorsInitializedParams& Params)
{
	// This delegate is global: it fires for every world in the process, editor
	// preview worlds included, so the game instance is the filter that makes it
	// this session's business.
	if (Params.World == nullptr || Params.World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	EnsureViewState(Params.World);
}

void UUSViewSubsystem::HandleGameInstanceWorldChanged(UGameInstance* OwningGameInstance, UWorld* OldWorld, UWorld* NewWorld)
{
	if (OwningGameInstance != GetGameInstance())
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("World changed: Old=%s New=%s"),
		OldWorld != nullptr ? *OldWorld->GetName() : TEXT("None"),
		NewWorld != nullptr ? *NewWorld->GetName() : TEXT("None"));

	CachedViewState.Reset();

	// The viewport client outlives the world, so without this a blend, a merge and
	// its saved pre-merge cameras would all carry into the next level and animate
	// from a layout that belonged to the last one. The new world's view state
	// pushes its own intent from here, and it snaps rather than blends.
	if (UUSGameViewportClient* const ViewportClient = GetUSViewportClient())
	{
		ViewportClient->ResetForNewWorld();
	}
}
