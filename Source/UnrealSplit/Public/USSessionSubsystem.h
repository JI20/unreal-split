// Copyright (c) ji20. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USSessionSubsystem.generated.h"

/**
 * Connection layer (host/join/travel) — implemented in step 1.2.
 *
 * Owns axis 1 of the UnrealSplit view model: whether the session runs as local
 * splitscreen or over the network. Game-instance scoped so it survives level
 * travel, which any session flow depends on.
 *
 * Session mode is deliberately invisible to the view system — both modes render
 * two view slots — so this subsystem also owns creating the shadow local player
 * that gives an online client a view of the remote player.
 */
UCLASS()
class UNREALSPLIT_API UUSSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface
};
