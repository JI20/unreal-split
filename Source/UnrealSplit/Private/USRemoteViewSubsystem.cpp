// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USRemoteViewSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"

#include "UnrealSplitModule.h"
#include "USPlayerViewProxy.h"
#include "USShadowLocalPlayer.h"
#include "USSpectatorPlayerController.h"

void UUSRemoteViewSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Authority side: every player that logs in gets a camera stream. Bound to the
	// global game mode event rather than to a plugin game mode base, so this works
	// whichever game mode the game uses.
	PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(this, &UUSRemoteViewSubsystem::HandleGameModePostLogin);
	LogoutHandle = FGameModeEvents::GameModeLogoutEvent.AddUObject(this, &UUSRemoteViewSubsystem::HandleGameModeLogout);

	if (UGameInstance* const GameInstance = GetGameInstance())
	{
		// A real player arriving or leaving changes the answer: two people at this
		// machine need both slots for themselves, and the shadow view has to give
		// its slot back.
		LocalPlayerAddedHandle = GameInstance->OnLocalPlayerAddedEvent.AddUObject(this, &UUSRemoteViewSubsystem::HandleLocalPlayerChanged);
		LocalPlayerRemovedHandle = GameInstance->OnLocalPlayerRemovedEvent.AddUObject(this, &UUSRemoteViewSubsystem::HandleLocalPlayerChanged);
	}

	// Torn down before the world goes, not after. UEngine::LoadMap spawns a play
	// actor for every local player still present and calls a failure fatal, and on
	// a listen server that spawn would be a real login. Both kinds of travel are
	// covered: a map load tears the old world down first, and seamless travel
	// announces itself.
	WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &UUSRemoteViewSubsystem::HandleWorldBeginTearDown);
	SeamlessTravelHandle = FWorldDelegates::OnSeamlessTravelStart.AddUObject(this, &UUSRemoteViewSubsystem::HandleSeamlessTravelStart);

	LastShadowViewNote = TEXT("no remote camera stream has delivered anything yet");

	UE_LOG(LogUnrealSplit, Log, TEXT("Remote view subsystem initialized (enabled=%d, shadow platform user index %d)."),
		bRemoteViewEnabled ? 1 : 0, ShadowPlatformUserIndex);
}

void UUSRemoteViewSubsystem::Deinitialize()
{
	DestroyShadowView(TEXT("the game instance is shutting down"));

	FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
	PostLoginHandle.Reset();

	FGameModeEvents::GameModeLogoutEvent.Remove(LogoutHandle);
	LogoutHandle.Reset();

	if (UGameInstance* const GameInstance = GetGameInstance())
	{
		GameInstance->OnLocalPlayerAddedEvent.Remove(LocalPlayerAddedHandle);
		GameInstance->OnLocalPlayerRemovedEvent.Remove(LocalPlayerRemovedHandle);
	}

	LocalPlayerAddedHandle.Reset();
	LocalPlayerRemovedHandle.Reset();

	FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
	WorldTearDownHandle.Reset();

	FWorldDelegates::OnSeamlessTravelStart.Remove(SeamlessTravelHandle);
	SeamlessTravelHandle.Reset();

	ViewProxies.Reset();

	Super::Deinitialize();
}

void UUSRemoteViewSubsystem::SetRemoteViewEnabled(bool bEnabled)
{
	if (bRemoteViewEnabled == bEnabled)
	{
		return;
	}

	bRemoteViewEnabled = bEnabled;
	UE_LOG(LogUnrealSplit, Log, TEXT("Remote view %s on this machine."), bEnabled ? TEXT("enabled") : TEXT("disabled"));

	EvaluateShadowView();
}

bool UUSRemoteViewSubsystem::IsShadowViewActive() const
{
	return ShadowController != nullptr && ShadowPlayer != nullptr;
}

AUSPlayerViewProxy* UUSRemoteViewSubsystem::GetShadowViewSource() const
{
	return ShadowSource.Get();
}

FString UUSRemoteViewSubsystem::DescribeShadowViewState() const
{
	if (IsShadowViewActive())
	{
		const AUSPlayerViewProxy* const Source = ShadowSource.Get();

		if (Source == nullptr)
		{
			return TEXT("active, but its stream has gone away");
		}

		// Four numbers that separate the failure modes at a glance:
		//   depth     — climbing toward the cap with a healthy Hz is fine; climbing
		//               while Hz collapses means arrivals are bunching after stalls.
		//   offset    — should sit near the target delay. Growing while sample age
		//               stays low is clock drift; both large means the process slept.
		//   Hz        — sender cadence as received. Well below ~30 means coalescing
		//               or a starved net tick, not a render-cost problem.
		//   sample age — wall time since the last arrival. Large with a stable
		//               offset means the stream stopped; the view is holding.
		const FString OffsetText = Source->HasLivePlayback()
			? FString::Printf(TEXT("%.0f ms"), Source->GetPlaybackOffsetSeconds() * 1000.0f)
			: FString(TEXT("not started"));

		return FString::Printf(
			TEXT("active from '%s' (buffer %d, offset %s / target %.0f ms, %.0f Hz received, last sample %.0f ms ago)"),
			*GetNameSafe(Source->GetOwningPlayerState()),
			Source->GetBufferedSampleCount(),
			*OffsetText,
			Source->GetInterpolationDelaySeconds() * 1000.0f,
			Source->GetMeasuredSampleRateHz(),
			Source->GetNewestSampleAgeSeconds() * 1000.0f);
	}

	return FString::Printf(TEXT("inactive — %s"), *LastShadowViewNote);
}

void UUSRemoteViewSubsystem::NotifyRemoteStreamLive(AUSPlayerViewProxy* Proxy)
{
	if (Proxy == nullptr)
	{
		return;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Remote camera stream '%s' is delivering (player '%s')."),
		*Proxy->GetName(), *GetNameSafe(Proxy->GetOwningPlayerState()));

	EvaluateShadowView();
}

void UUSRemoteViewSubsystem::NotifyRemoteStreamLost(AUSPlayerViewProxy* Proxy)
{
	if (Proxy == nullptr)
	{
		return;
	}

	ViewProxies.Remove(Proxy->GetOwner<APlayerController>());

	if (ShadowSource.Get() == Proxy)
	{
		DestroyShadowView(TEXT("the remote camera stream ended"));
		LastShadowViewNote = TEXT("the remote camera stream ended");
	}

	EvaluateShadowView();

	// A side just became free, which is a change a picker has to redraw even though
	// nobody moved: the slot is still claimed for a reconnect, but no live stream
	// occupies it any more.
	NotifyViewSlotsChanged();
}

UWorld* UUSRemoteViewSubsystem::GetOwningWorld() const
{
	const UGameInstance* const GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetWorld() : nullptr;
}

void UUSRemoteViewSubsystem::EvaluateShadowView()
{
	if (bMutatingShadowView)
	{
		return;
	}

	UGameInstance* const GameInstance = GetGameInstance();
	UWorld* const World = GetOwningWorld();

	if (!bRemoteViewEnabled)
	{
		DestroyShadowView(TEXT("the remote view was switched off"));
		LastShadowViewNote = TEXT("switched off on this machine (US.RemoteView on)");
		return;
	}

	if (World == nullptr || !World->IsGameWorld())
	{
		DestroyShadowView(TEXT("there is no game world"));
		LastShadowViewNote = TEXT("there is no game world");
		return;
	}

	// Not a session-mode branch: standalone has no remote player to show and a
	// dedicated server draws nothing at all, so in both cases there is simply
	// nothing that could fill a second slot. LAN and EOS are not distinguished
	// anywhere here.
	const ENetMode NetMode = World->GetNetMode();
	if (NetMode == NM_Standalone || NetMode == NM_DedicatedServer)
	{
		DestroyShadowView(TEXT("this machine is not in a networked session with a remote player"));
		LastShadowViewNote = TEXT("this machine is standalone, so there is no remote player to show");
		return;
	}

	const int32 RealLocalPlayers = UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance);
	if (RealLocalPlayers != 1)
	{
		DestroyShadowView(TEXT("this machine does not have exactly one real local player"));
		LastShadowViewNote = FString::Printf(TEXT("%d people are playing at this machine, so both slots are already theirs"), RealLocalPlayers);
		return;
	}

	AUSPlayerViewProxy* const Source = FindRemoteStream();
	if (Source == nullptr)
	{
		DestroyShadowView(TEXT("no remote camera stream is delivering"));
		LastShadowViewNote = TEXT("no remote camera stream is delivering yet");
		return;
	}

	if (IsShadowViewActive())
	{
		if (ShadowSource.Get() != Source)
		{
			// A different partner, same slot: repointing is cheaper and less
			// disruptive than tearing the view down and building it again.
			ShadowSource = Source;
			ShadowController->SetViewProxy(Source);

			UE_LOG(LogUnrealSplit, Log, TEXT("Shadow view repointed at stream '%s'."), *Source->GetName());
		}

		return;
	}

	CreateShadowView(Source);
}

bool UUSRemoteViewSubsystem::CreateShadowView(AUSPlayerViewProxy* Source)
{
	UGameInstance* const GameInstance = GetGameInstance();
	UWorld* const World = GetOwningWorld();

	if (GameInstance == nullptr || World == nullptr || Source == nullptr)
	{
		return false;
	}

	// The engine's own cap on views in one viewport. AddLocalPlayer below does not
	// check it — UGameInstance::CreateLocalPlayer does, and this deliberately does
	// not go through that — so a project configured for a single view would end up
	// with a player the layout has no room for.
	const UGameViewportClient* const Viewport = GameInstance->GetGameViewportClient();
	const int32 MaxViews = Viewport != nullptr ? Viewport->MaxSplitscreenPlayers : 1;

	if (GameInstance->GetNumLocalPlayers() >= MaxViews)
	{
		LastShadowViewNote = FString::Printf(TEXT("this viewport allows %d view(s) and already has them"), MaxViews);
		UE_LOG(LogUnrealSplit, Warning, TEXT("Not creating a shadow view: %s."), *LastShadowViewNote);
		return false;
	}

	const FPlatformUserId ShadowUserId = FindUnusedPlatformUserId();
	if (!ShadowUserId.IsValid())
	{
		LastShadowViewNote = TEXT("no platform user id is free for a view that must never receive input");
		UE_LOG(LogUnrealSplit, Warning, TEXT("Not creating a shadow view: %s. The machine stays full screen."), *LastShadowViewNote);
		return false;
	}

	// Guards the local-player events this is about to raise: adding the shadow
	// player broadcasts the same event that asks whether it should exist.
	TGuardValue<bool> MutationGuard(bMutatingShadowView, true);

	// Built by hand rather than through UGameInstance::CreateLocalPlayer, which
	// always instantiates UEngine::LocalPlayerClass and would give a plain
	// ULocalPlayer with none of the guards this needs — and which sends a split
	// join on a client.
	UUSShadowLocalPlayer* const NewShadowPlayer = NewObject<UUSShadowLocalPlayer>(GEngine, UUSShadowLocalPlayer::StaticClass());
	const int32 LocalPlayerIndex = GameInstance->AddLocalPlayer(NewShadowPlayer, ShadowUserId);

	if (LocalPlayerIndex == INDEX_NONE)
	{
		LastShadowViewNote = TEXT("the engine refused to add the shadow local player");
		UE_LOG(LogUnrealSplit, Warning, TEXT("Not creating a shadow view: %s."), *LastShadowViewNote);
		return false;
	}

	FActorSpawnParameters SpawnInfo;
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AUSSpectatorPlayerController* const NewController = World->SpawnActor<AUSSpectatorPlayerController>(SpawnInfo);

	if (NewController == nullptr)
	{
		GameInstance->RemoveLocalPlayer(NewShadowPlayer);
		LastShadowViewNote = TEXT("the spectator controller could not be spawned");
		UE_LOG(LogUnrealSplit, Warning, TEXT("Not creating a shadow view: %s."), *LastShadowViewNote);
		return false;
	}

	// Binds the two together: this is what marks the controller as local, which is
	// what makes the engine tick its camera manager and hand it a scene view.
	NewController->SetPlayer(NewShadowPlayer);
	NewController->SetViewProxy(Source);

	ShadowPlayer = NewShadowPlayer;
	ShadowController = NewController;
	ShadowSource = Source;
	LastShadowViewNote.Reset();

	// Two different numbers, and it is worth logging both: the local player index is
	// where the engine filed this view, while the view slot is where the layout will
	// draw it — the authority chose that one for the streamed player, so on the two
	// machines the same player lands on the same side.
	UE_LOG(LogUnrealSplit, Log, TEXT("Shadow view created as local player %d for stream '%s' (player '%s', view slot %d), platform user %d. This machine now renders %d view(s)."),
		LocalPlayerIndex,
		*Source->GetName(),
		*GetNameSafe(Source->GetOwningPlayerState()),
		Source->GetViewSlotIndex(),
		ShadowUserId.GetInternalId(),
		GameInstance->GetNumLocalPlayers());

	return true;
}

void UUSRemoteViewSubsystem::DestroyShadowView(const TCHAR* Reason)
{
	if (ShadowPlayer == nullptr && ShadowController == nullptr)
	{
		return;
	}

	TGuardValue<bool> MutationGuard(bMutatingShadowView, true);

	UGameInstance* const GameInstance = GetGameInstance();

	// RemoveLocalPlayer destroys the player's controller when that controller has
	// authority, which a locally spawned one always does, so this normally takes
	// the spectator controller with it.
	if (GameInstance != nullptr && ShadowPlayer != nullptr)
	{
		GameInstance->RemoveLocalPlayer(ShadowPlayer);
	}

	if (AUSSpectatorPlayerController* const Controller = ShadowController.Get())
	{
		if (IsValid(Controller))
		{
			Controller->Destroy();
		}
	}

	ShadowController = nullptr;
	ShadowPlayer = nullptr;
	ShadowSource = nullptr;

	UE_LOG(LogUnrealSplit, Log, TEXT("Shadow view destroyed: %s. This machine now renders %d view(s)."),
		Reason,
		GameInstance != nullptr ? GameInstance->GetNumLocalPlayers() : 0);
}

AUSPlayerViewProxy* UUSRemoteViewSubsystem::FindRemoteStream() const
{
	UWorld* const World = GetOwningWorld();

	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<AUSPlayerViewProxy> It(World); It; ++It)
	{
		AUSPlayerViewProxy* const Proxy = *It;

		// "Delivering to this machine" rather than "owned by someone else", because
		// a stream only ever delivers to machines that did not produce it: the
		// owning machine is skipped by the replication condition and never receives
		// its own samples. That makes this immune to whether an owner reference has
		// resolved yet.
		if (Proxy != nullptr && !Proxy->IsSendingMachine() && Proxy->GetBufferedSampleCount() > 0)
		{
			return Proxy;
		}
	}

	return nullptr;
}

FPlatformUserId UUSRemoteViewSubsystem::FindUnusedPlatformUserId() const
{
	const UGameInstance* const GameInstance = GetGameInstance();

	if (GameInstance == nullptr)
	{
		return PLATFORMUSERID_NONE;
	}

	IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();

	// A handful of indices above the requested one, in case a machine really does
	// have that many devices paired.
	for (int32 Index = ShadowPlatformUserIndex; Index < ShadowPlatformUserIndex + 8; ++Index)
	{
		const FPlatformUserId Candidate = Mapper.GetPlatformUserForUserIndex(Index);

		if (!Candidate.IsValid() || GameInstance->FindLocalPlayerFromPlatformUserId(Candidate) != nullptr)
		{
			continue;
		}

		TArray<FInputDeviceId> PairedDevices;
		Mapper.GetAllConnectedInputDevicesForUser(Candidate, PairedDevices);

		if (PairedDevices.Num() > 0)
		{
			UE_LOG(LogUnrealSplit, Verbose, TEXT("Platform user %d already has %d input device(s); trying the next one for the shadow view."),
				Candidate.GetInternalId(), PairedDevices.Num());
			continue;
		}

		return Candidate;
	}

	return PLATFORMUSERID_NONE;
}

void UUSRemoteViewSubsystem::HandleGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
{
	// The events are global, and in the editor several game instances can be
	// running at once, so anything from another one is not this machine's business.
	if (GameMode == nullptr || GameMode->GetWorld() != GetOwningWorld())
	{
		return;
	}

	EnsureViewProxyForPlayer(NewPlayer);
	EvaluateShadowView();
}

void UUSRemoteViewSubsystem::HandleGameModeLogout(AGameModeBase* GameMode, AController* Exiting)
{
	APlayerController* const PlayerController = Cast<APlayerController>(Exiting);

	if (PlayerController == nullptr)
	{
		return;
	}

	// Looked up by value rather than held as a pointer into the map: destroying the
	// proxy runs its EndPlay synchronously, which comes back through
	// NotifyRemoteStreamLost and takes the entry out from under us.
	if (AUSPlayerViewProxy* const ProxyActor = ViewProxies.FindRef(PlayerController).Get())
	{
		// Destroying it is what tells the other machine to drop the view: the
		// proxy's EndPlay runs there too.
		ProxyActor->Destroy();
	}

	ViewProxies.Remove(PlayerController);
	EvaluateShadowView();
}

void UUSRemoteViewSubsystem::HandleLocalPlayerChanged(ULocalPlayer* Player)
{
	if (UUSShadowLocalPlayer::IsShadow(Player))
	{
		return;
	}

	EvaluateShadowView();
}

void UUSRemoteViewSubsystem::HandleWorldBeginTearDown(UWorld* World)
{
	if (World == nullptr || World != GetOwningWorld())
	{
		return;
	}

	ViewProxies.Reset();
	DestroyShadowView(TEXT("the world is being torn down"));
	LastShadowViewNote = TEXT("waiting for the next world");
}

void UUSRemoteViewSubsystem::HandleSeamlessTravelStart(UWorld* World, const FString& TravelUrl)
{
	if (World == nullptr || World != GetOwningWorld())
	{
		return;
	}

	ViewProxies.Reset();
	DestroyShadowView(TEXT("seamless travel is starting"));
	LastShadowViewNote = TEXT("travelling");
}

void UUSRemoteViewSubsystem::EnsureViewProxyForPlayer(APlayerController* PlayerController)
{
	UWorld* const World = GetOwningWorld();

	if (World == nullptr || PlayerController == nullptr)
	{
		return;
	}

	// A shadow view's controller never logs in, so it should never arrive here;
	// refusing it explicitly means a future path that does log one in produces no
	// stream rather than a stream of a view of a view.
	if (UUSShadowLocalPlayer::IsShadowController(PlayerController))
	{
		return;
	}

	if (const TObjectPtr<AUSPlayerViewProxy>* const Existing = ViewProxies.Find(PlayerController))
	{
		if (Existing->Get() != nullptr)
		{
			return;
		}
	}

	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Owner = PlayerController;
	SpawnInfo.ObjectFlags |= RF_Transient;

	AUSPlayerViewProxy* const Proxy = World->SpawnActor<AUSPlayerViewProxy>(SpawnInfo);

	if (Proxy == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("Could not spawn a camera stream for '%s'; the other machine will not see this player."), *PlayerController->GetName());
		return;
	}

	const int32 SlotIndex = AssignViewSlot(PlayerController);

	Proxy->InitializeForPlayer(PlayerController, SlotIndex);
	ViewProxies.Add(PlayerController, Proxy);

	UE_LOG(LogUnrealSplit, Log, TEXT("Camera stream '%s' created for '%s' in view slot %d (%d stream(s) in this world)."),
		*Proxy->GetName(), *PlayerController->GetName(), SlotIndex, ViewProxies.Num());

	NotifyViewSlotsChanged();
}

bool UUSRemoteViewSubsystem::IsViewSlotTaken(int32 SlotIndex) const
{
	for (const TPair<TObjectPtr<APlayerController>, TObjectPtr<AUSPlayerViewProxy>>& Pair : ViewProxies)
	{
		const AUSPlayerViewProxy* const Proxy = Pair.Value.Get();

		if (Proxy != nullptr && Proxy->GetViewSlotIndex() == SlotIndex)
		{
			return true;
		}
	}

	return false;
}

int32 UUSRemoteViewSubsystem::AssignViewSlot(const APlayerController* PlayerController)
{
	const FViewSlotOwner Owner = MakeSlotOwnerKey(PlayerController);

	if (Owner.IsValid())
	{
		const int32 PreviousSlot = FindClaimedViewSlot(Owner);

		// Somebody else may have been given the slot while this player was away, in
		// which case the old claim is stale and it takes a free one below.
		if (PreviousSlot != INDEX_NONE && !IsViewSlotTaken(PreviousSlot))
		{
			return PreviousSlot;
		}
	}

	int32 FreeSlot = 0;

	while (IsViewSlotTaken(FreeSlot))
	{
		++FreeSlot;
	}

	ClaimViewSlot(FreeSlot, Owner);

	return FreeSlot;
}

UUSRemoteViewSubsystem::FViewSlotOwner UUSRemoteViewSubsystem::MakeSlotOwnerKey(const APlayerController* PlayerController) const
{
	FViewSlotOwner Owner;

	if (PlayerController == nullptr)
	{
		return Owner;
	}

	if (const APlayerState* const PlayerState = PlayerController->PlayerState)
	{
		Owner.Identity = PlayerState->GetUniqueId();
	}

	// Only for a player of this machine. A remote player's platform user belongs to
	// their machine and means nothing here, so leaving it unset is what makes the
	// match below fall through to the online id for them.
	if (const ULocalPlayer* const LocalPlayer = PlayerController->GetLocalPlayer())
	{
		Owner.LocalUser = LocalPlayer->GetPlatformUserId();
	}

	return Owner;
}

bool UUSRemoteViewSubsystem::DoSlotOwnersMatch(const FViewSlotOwner& First, const FViewSlotOwner& Second)
{
	// Platform user first, and only when both sides have one: it is the key that
	// stays distinct between two people sitting at the same machine, which is
	// precisely where the online id cannot be trusted to.
	if (First.LocalUser.IsValid() && Second.LocalUser.IsValid())
	{
		return First.LocalUser == Second.LocalUser;
	}

	return First.Identity.IsValid() && First.Identity == Second.Identity;
}

void UUSRemoteViewSubsystem::ClaimViewSlot(int32 SlotIndex, const FViewSlotOwner& Owner)
{
	if (SlotIndex < 0)
	{
		return;
	}

	if (!ViewSlotOwners.IsValidIndex(SlotIndex))
	{
		ViewSlotOwners.SetNum(SlotIndex + 1);
	}

	ViewSlotOwners[SlotIndex] = Owner;
}

int32 UUSRemoteViewSubsystem::FindClaimedViewSlot(const FViewSlotOwner& Owner) const
{
	if (!Owner.IsValid())
	{
		return INDEX_NONE;
	}

	return ViewSlotOwners.IndexOfByPredicate(
		[&Owner](const FViewSlotOwner& Existing)
		{
			return Existing.IsValid() && DoSlotOwnersMatch(Existing, Owner);
		});
}

AUSPlayerViewProxy* UUSRemoteViewSubsystem::FindViewProxyForPlayer(const APlayerController* PlayerController) const
{
	if (PlayerController == nullptr)
	{
		return nullptr;
	}

	// The authority spawned every stream and remembers them, so this is exact there.
	if (const TObjectPtr<AUSPlayerViewProxy>* const Owned = ViewProxies.Find(PlayerController))
	{
		if (AUSPlayerViewProxy* const Proxy = Owned->Get())
		{
			return Proxy;
		}
	}

	// A client has no such table, but a stream is always relevant and carries the
	// player it belongs to, so its own side is readable from the world. Matched on
	// the player state rather than the owner, because a client only ever sees its
	// own controller replicated.
	UWorld* const World = GetOwningWorld();
	const APlayerState* const PlayerState = PlayerController->PlayerState;

	if (World == nullptr || PlayerState == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<AUSPlayerViewProxy> It(World); It; ++It)
	{
		if (It->GetOwningPlayerState() == PlayerState)
		{
			return *It;
		}
	}

	return nullptr;
}

int32 UUSRemoteViewSubsystem::GetPlayerViewSlot(const APlayerController* PlayerController) const
{
	if (const AUSPlayerViewProxy* const Proxy = FindViewProxyForPlayer(PlayerController))
	{
		return Proxy->GetViewSlotIndex();
	}

	// No stream yet. On the authority a claim may already exist — a player that
	// logged in and had a slot recorded before its proxy was spawned, or one that
	// travelled — and answering from it keeps a pawn spawn from falling back to
	// join order for a frame.
	const int32 ClaimedSlot = FindClaimedViewSlot(MakeSlotOwnerKey(PlayerController));

	return ClaimedSlot;
}

bool UUSRemoteViewSubsystem::SetPlayerViewSlot(APlayerController* PlayerController, int32 SlotIndex)
{
	UWorld* const World = GetOwningWorld();

	if (World == nullptr || PlayerController == nullptr || SlotIndex < 0)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SetPlayerViewSlot rejected: no world, no player, or a negative slot."));
		return false;
	}

	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SetPlayerViewSlot rejected: view slots are decided by the authority."));
		return false;
	}

	AUSPlayerViewProxy* const Proxy = FindViewProxyForPlayer(PlayerController);

	if (Proxy == nullptr)
	{
		// No stream to carry it yet, but the claim is still worth recording: the
		// proxy reads it back through AssignViewSlot the moment it is spawned.
		ClaimViewSlot(SlotIndex, MakeSlotOwnerKey(PlayerController));
		UE_LOG(LogUnrealSplit, Log, TEXT("View slot %d recorded for '%s' before its camera stream exists."), SlotIndex, *PlayerController->GetName());
		NotifyViewSlotsChanged();
		return true;
	}

	if (Proxy->GetViewSlotIndex() == SlotIndex)
	{
		return true;
	}

	if (IsViewSlotTaken(SlotIndex))
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SetPlayerViewSlot rejected: view slot %d already belongs to another player; swap the two instead."), SlotIndex);
		return false;
	}

	Proxy->SetViewSlotIndex(SlotIndex);
	ClaimViewSlot(SlotIndex, MakeSlotOwnerKey(PlayerController));

	UE_LOG(LogUnrealSplit, Log, TEXT("'%s' moved to view slot %d."), *PlayerController->GetName(), SlotIndex);

	NotifyViewSlotsChanged();

	return true;
}

bool UUSRemoteViewSubsystem::SwapPlayerViewSlots(APlayerController* FirstPlayer, APlayerController* SecondPlayer)
{
	UWorld* const World = GetOwningWorld();

	if (World == nullptr || FirstPlayer == nullptr || SecondPlayer == nullptr || FirstPlayer == SecondPlayer)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SwapPlayerViewSlots rejected: two different players are needed."));
		return false;
	}

	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SwapPlayerViewSlots rejected: view slots are decided by the authority."));
		return false;
	}

	AUSPlayerViewProxy* const FirstProxy = FindViewProxyForPlayer(FirstPlayer);
	AUSPlayerViewProxy* const SecondProxy = FindViewProxyForPlayer(SecondPlayer);

	if (FirstProxy == nullptr || SecondProxy == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SwapPlayerViewSlots rejected: one of the players has no camera stream yet."));
		return false;
	}

	const int32 FirstSlot = FirstProxy->GetViewSlotIndex();
	const int32 SecondSlot = SecondProxy->GetViewSlotIndex();

	if (FirstSlot == INDEX_NONE || SecondSlot == INDEX_NONE)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SwapPlayerViewSlots rejected: one of the players has no view slot yet."));
		return false;
	}

	FirstProxy->SetViewSlotIndex(SecondSlot);
	SecondProxy->SetViewSlotIndex(FirstSlot);

	ClaimViewSlot(SecondSlot, MakeSlotOwnerKey(FirstPlayer));
	ClaimViewSlot(FirstSlot, MakeSlotOwnerKey(SecondPlayer));

	UE_LOG(LogUnrealSplit, Log, TEXT("Swapped view slots: '%s' now %d, '%s' now %d."),
		*FirstPlayer->GetName(), SecondSlot, *SecondPlayer->GetName(), FirstSlot);

	NotifyViewSlotsChanged();

	return true;
}

void UUSRemoteViewSubsystem::NotifyViewSlotsChanged()
{
	OnViewSlotsChanged.Broadcast();
}

void UUSRemoteViewSubsystem::SetViewSlotLabel(int32 SlotIndex, const FText& Label)
{
	if (SlotIndex < 0)
	{
		return;
	}

	if (!ViewSlotLabels.IsValidIndex(SlotIndex))
	{
		ViewSlotLabels.SetNum(SlotIndex + 1);
	}

	ViewSlotLabels[SlotIndex] = Label;
}

FText UUSRemoteViewSubsystem::GetViewSlotLabel(int32 SlotIndex) const
{
	if (ViewSlotLabels.IsValidIndex(SlotIndex) && !ViewSlotLabels[SlotIndex].IsEmpty())
	{
		return ViewSlotLabels[SlotIndex];
	}

	// Two views is the whole range this game can produce, so naming the sides is a
	// complete fallback rather than a placeholder.
	switch (SlotIndex)
	{
	case 0:
		return NSLOCTEXT("UnrealSplit", "ViewSlotLeft", "Left");
	case 1:
		return NSLOCTEXT("UnrealSplit", "ViewSlotRight", "Right");
	default:
		return FText::AsNumber(SlotIndex);
	}
}
