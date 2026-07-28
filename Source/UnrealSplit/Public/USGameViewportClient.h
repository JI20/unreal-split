// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "USGameViewportClient.generated.h"

/**
 * Viewport client for UnrealSplit.
 *
 * Engine splitscreen only offers fixed layouts, so UnrealSplit takes over the
 * per-player viewport rectangles here to support arbitrary and animated split
 * ratios. Set as GameViewportClientClassName in the project's DefaultEngine.ini.
 *
 * Lays out two view slots unconditionally: online clients render a shadow view
 * of the remote player alongside their own, so layout is identical to couch
 * co-op and must never branch on session mode.
 */
UCLASS()
class UNREALSPLIT_API UUSGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	//~ Begin UGameViewportClient interface
	virtual void LayoutPlayers() override;
	//~ End UGameViewportClient interface
};
