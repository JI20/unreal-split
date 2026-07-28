// Copyright (c) ji20. All Rights Reserved.

#include "USGameViewportClient.h"

void UUSGameViewportClient::LayoutPlayers()
{
	// The animated per-frame split-ratio layout driven by UUSViewSubsystem will
	// replace this call by writing each local player's Origin/Size directly.
	Super::LayoutPlayers();
}
