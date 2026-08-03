// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USSpectatorPlayerController.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/SpectatorPawn.h"

#include "UnrealSplitModule.h"
#include "USPlayerViewProxy.h"
#include "USShadowLocalPlayer.h"

AUSSpectatorPlayerController::AUSSpectatorPlayerController()
{
	// Local only. A player controller normally replicates, and although the base
	// class already reports itself relevant to nobody but its own viewer, saying
	// so here keeps it out of the replication path entirely.
	bReplicates = false;

	// This controller is its own view target and CalcCamera below is the view.
	// Left on, the base class would hand the view to any level camera whose
	// auto-activate index happened to match and the partner would vanish.
	bAutoManageActiveCameraTarget = false;

	// A pause on this machine is a pause of this machine's game; the partner keeps
	// playing and their view has to keep showing it. Ticking through the pause is
	// what lets that happen, and it is safe because the only thing this tick does
	// is read a buffer of samples and move a camera.
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void AUSSpectatorPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// Out of the world's player-controller list, which AController::
	// PostInitializeComponents put it in. This is the single most effective
	// exclusion in the whole feature, because that list is what almost everything
	// means by "the players":
	//
	//   - AGameModeBase::GetNumPlayers counts every controller in it that is not
	//     spectating, and a controller with no player state does not read as a
	//     spectator, so on a listen server this would otherwise consume a seat in
	//     AGameSession::AtCapacity and could refuse a real player.
	//   - UGameplayStatics::GetPlayerController indexes into it, so game code
	//     asking for "player 2" would be handed a controller that renders somebody
	//     else's camera and answers no input.
	//   - AGameMode's restart loops walk it looking for controllers without pawns.
	//   - The seamless travel handler walks it to decide who travels.
	//
	// Nothing is lost except the camera update UWorld::Tick performs for the
	// controllers in that list, which Tick below takes over. Rendering is
	// unaffected: the viewport draws local players and reaches their controllers
	// through ULocalPlayer::PlayerController, never through this list.
	if (UWorld* const World = GetWorld())
	{
		World->RemoveController(this);
	}
}

void AUSSpectatorPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The camera update this controller no longer receives from UWorld::Tick,
	// which is what fills the camera cache the renderer reads through
	// ULocalPlayer::GetViewPoint. One tick group earlier than the engine's own
	// pass, and none of it matters: this view is a replay of another machine's
	// camera from a tenth of a second ago, not a reaction to anything local.
	UpdateCameraManager(DeltaSeconds);
}

void AUSSpectatorPlayerController::SetViewProxy(AUSPlayerViewProxy* Proxy)
{
	ViewProxy = Proxy;
	bHasKnownView = false;

	// Its own view target, so APlayerCameraManager::UpdateViewTarget resolves the
	// view through CalcCamera rather than through a pawn or a camera component.
	SetViewTarget(this);
}

AUSPlayerViewProxy* AUSSpectatorPlayerController::GetViewProxy() const
{
	return ViewProxy.Get();
}

void AUSSpectatorPlayerController::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	AUSPlayerViewProxy* const Proxy = ViewProxy.Get();

	if (Proxy != nullptr && Proxy->GetInterpolatedView(OutResult))
	{
		LastKnownView = OutResult;
		bHasKnownView = true;

		// Passed on to the renderer so it does not blend across the cut: without
		// this, temporal anti-aliasing and motion blur smear the two framings
		// together for several frames.
		if (Proxy->ConsumePendingCameraCut() && PlayerCameraManager != nullptr)
		{
			PlayerCameraManager->SetGameCameraCutThisFrame();
		}

		return;
	}

	if (bHasKnownView)
	{
		OutResult = LastKnownView;
		return;
	}

	Super::CalcCamera(DeltaTime, OutResult);
}

bool AUSSpectatorPlayerController::CanRestartPlayer()
{
	// The answer the base class would give anyway, since it needs a player state
	// and this has none. Said out loud because this is the question a listen
	// server's game mode asks before it spawns a pawn for a controller — and a
	// pawn here would be a second character in the level, replicated to the
	// partner, controlled by nobody.
	return false;
}

void AUSSpectatorPlayerController::GetAudioListenerPosition(FVector& OutLocation, FVector& OutFrontDir, FVector& OutRightDir) const
{
	// UGameViewportClient::Draw sets an audio listener for every local player that
	// has a controller, and there is no flag to opt out of that — so instead of
	// suppressing this machine's second listener, it is placed exactly on top of
	// the first. Attenuation picks the nearest listener, and two in the same place
	// give the same mix as one, so the partner's half of the screen adds no ears
	// to the level. Reporting this controller's own position would let sounds near
	// the partner be heard as if they were next to the player.
	if (const APlayerController* const RealController = UUSShadowLocalPlayer::GetFirstRealLocalPlayerController(GetGameInstance()))
	{
		if (RealController != this)
		{
			RealController->GetAudioListenerPosition(OutLocation, OutFrontDir, OutRightDir);
			return;
		}
	}

	Super::GetAudioListenerPosition(OutLocation, OutFrontDir, OutRightDir);
}

void AUSSpectatorPlayerController::InitPlayerState()
{
	// Deliberately nothing. A player state would put this controller into the
	// game state's player array, register it with the online session on a listen
	// server, and make AGameModeBase::GetNumPlayers count it as a player.
}

void AUSSpectatorPlayerController::InitInputSystem()
{
	// Deliberately nothing, and this is the whole of the input exclusion.
	//
	// Not calling the base class leaves PlayerInput null, and that is what makes
	// input impossible rather than merely unrouted: APlayerController::TickActor
	// only runs PlayerTick when PlayerInput exists, so this controller never
	// processes an input stack, and UPlayerInput is the only thing that could
	// translate a key into an action. It has no input component to bind to either.
	//
	// The base class also does something less obvious that has to be avoided:
	// ULevel::PushPendingAutoReceiveInput hands every level actor whose Auto
	// Receive Input matches this controller's *index* among the world's player
	// controllers over to it. A shadow view sits at index 1, which is where the
	// second real player would be, so an actor authored to take player two's input
	// would bind to a controller that can never deliver any — and, worse, would no
	// longer be waiting when a real second player arrived.
	//
	// The engine calls this again every tick while PlayerInput is null, which
	// costs an empty virtual call and is the price of the guarantee.
}

void AUSSpectatorPlayerController::SpawnDefaultHUD()
{
	// Deliberately nothing. The shadow view is a window onto someone else's
	// screen; its own HUD would be a second, empty one drawn over it.
}

ASpectatorPawn* AUSSpectatorPlayerController::SpawnSpectatorPawn()
{
	// Deliberately nothing. A spectator pawn is a real, replicated actor: spawned
	// on a listen server it would show up in the level on the other machine.
	return nullptr;
}
