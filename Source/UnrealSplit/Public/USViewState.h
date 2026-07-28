// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "USViewTypes.h"
#include "USViewState.generated.h"

/**
 * Server-authoritative carrier for the shared view intent (axis 3 + merge state).
 *
 * Perspective changes what players can do, not just what they see, so the
 * decision lives on the server and replicates down; clients only render it.
 * Spawned by UUSViewSubsystem on the authority once per world, and always
 * relevant so both players receive it regardless of position.
 *
 * The setters below are authority-only. There is intentionally no client
 * request path: view changes are triggered by level volumes and puzzle logic,
 * which already run on the server. A client can't Server-RPC through this
 * actor anyway since it owns no connection — if player-initiated view requests
 * are ever needed, they must route through the owning PlayerController.
 *
 * Skeleton only: setters log and do not mutate state yet.
 */
UCLASS()
class UNREALSPLIT_API AUSViewState : public AInfo
{
	GENERATED_BODY()

public:
	AUSViewState();

	//~ Begin AActor interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	/** Current replicated view intent. Valid on both server and clients. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|View")
	FUSViewIntent GetViewIntent() const { return ViewIntent; }

	/**
	 * Sets the camera perspective. Server only.
	 * @param Perspective Target perspective.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 */
	void SetPerspective(EUSPerspective Perspective, float BlendTime);

	/**
	 * Sets the share of the screen given to player 1 while split. Server only.
	 * @param Player1Share Fraction of the screen for player 1, in the range 0..1.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 */
	void SetSplitRatio(float Player1Share, float BlendTime);

	/**
	 * Collapses both players onto a single shared camera. Server only.
	 * @param Policy Which camera drives the merged view.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 */
	void SetMerged(EUSMergedCameraPolicy Policy, float BlendTime);

	/**
	 * Returns both players to their own cameras. Server only.
	 * @param Player1Share Fraction of the screen for player 1, in the range 0..1.
	 * @param BlendTime Seconds to blend; 0 applies instantly.
	 */
	void ClearMerged(float Player1Share, float BlendTime);

protected:
	/** Applies the newly replicated intent to the local view subsystem. */
	UFUNCTION()
	void OnRep_ViewIntent();

	UPROPERTY(ReplicatedUsing = OnRep_ViewIntent, BlueprintReadOnly, Category = "UnrealSplit|View")
	FUSViewIntent ViewIntent;
};
