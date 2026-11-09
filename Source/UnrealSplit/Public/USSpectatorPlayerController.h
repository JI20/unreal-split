// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/PlayerController.h"
#include "USViewTypes.h"
#include "USSpectatorPlayerController.generated.h"

class AUSPlayerViewProxy;
class ASpectatorPawn;

/**
 * Drives the shadow view: a controller that exists on one machine, controls
 * nothing, and reports whatever the partner's camera stream says.
 *
 * Spawned locally and never replicated, so it is not a player in the session by
 * any measure the game or the online layer uses. Everything that would otherwise
 * make it look like one is refused here, and each refusal is a specific engine
 * path rather than a precaution:
 *
 *   - Not in the world's player-controller list, which is what the game mode, the
 *     game session's capacity check, UGameplayStatics and the travel handler all
 *     mean by "the players". Leaving it takes over one job, the per-frame camera
 *     update, and is what makes the exclusion structural instead of a list of
 *     places that remembered to filter.
 *
 *   - No player state. APlayerController::PostInitializeComponents calls
 *     InitPlayerState on anything that is not a client, so on a listen server
 *     this controller would otherwise spawn one, and a player state adds itself
 *     to the game state's player array and registers with the online session.
 *     AGameModeBase::GetNumPlayers counts controllers that have a player state,
 *     which makes suppressing it the single lever that keeps this out of every
 *     capacity check at once.
 *   - No input system at all: no UPlayerInput, so the engine skips PlayerTick and
 *     there is nothing that could turn a key into an action, and no level actor's
 *     auto-receive input is handed to it. No device is paired with its local
 *     player either, so nothing is routed here in the first place.
 *   - No spectator pawn. The base class would spawn one on state change, and a
 *     pawn is a replicated actor: on a listen server it would appear in the level
 *     on the other machine.
 *   - No HUD, which the base class spawns on clients.
 *   - No automatic camera target, which would otherwise hand the view to a level
 *     camera with a matching auto-activate index and quietly stop showing the
 *     partner.
 *   - The audio listener is reported at the real player's ears rather than here,
 *     unless this machine's policy is Shared, in which case this view is a second
 *     set of ears at the partner's camera (US.AudioMode).
 */
UCLASS(NotPlaceable)
class UNREALSPLIT_API AUSSpectatorPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AUSSpectatorPlayerController();

	/** Points this controller at the stream it should render. */
	void SetViewProxy(AUSPlayerViewProxy* Proxy);

	AUSPlayerViewProxy* GetViewProxy() const;

	//~ Begin AActor interface
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
	//~ End AActor interface

	//~ Begin APlayerController interface
	virtual bool CanRestartPlayer() override;
	virtual void GetAudioListenerPosition(FVector& OutLocation, FVector& OutFrontDir, FVector& OutRightDir) const override;
	virtual void InitInputSystem() override;
	virtual void InitPlayerState() override;
	virtual void SpawnDefaultHUD() override;
	virtual ASpectatorPawn* SpawnSpectatorPawn() override;
	//~ End APlayerController interface

private:
	/**
	 * Copies the parts of the projection a stream does not carry from this
	 * machine's own camera, so both views are framed by the same rules.
	 */
	void ApplyLocalProjectionSettings(FMinimalViewInfo& OutResult) const;

	/** This machine's listener policy, or Own when the plugin's viewport client is absent. */
	EUSAudioListenerMode GetResolvedAudioListenerMode() const;

	UPROPERTY()
	TObjectPtr<AUSPlayerViewProxy> ViewProxy;

	/**
	 * The last view the stream produced, held for the frames when it produces
	 * none. Rendering a stale frame of somebody else's camera is unnoticeable;
	 * rendering the origin looking down the X axis is not.
	 */
	FMinimalViewInfo LastKnownView;
	bool bHasKnownView = false;
};
