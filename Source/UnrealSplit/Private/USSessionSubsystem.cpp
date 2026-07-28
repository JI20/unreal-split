// Copyright (c) ji20. All Rights Reserved.

#include "USSessionSubsystem.h"

#include "UnrealSplitModule.h"

void UUSSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogUnrealSplit, Log, TEXT("Session subsystem initialized."));
}

void UUSSessionSubsystem::Deinitialize()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("Session subsystem deinitialized."));

	Super::Deinitialize();
}
