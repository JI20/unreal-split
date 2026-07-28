// Copyright (c) Jonas Itt. All Rights Reserved.

#include "UnrealSplitModule.h"

#define LOCTEXT_NAMESPACE "FUnrealSplitModule"

DEFINE_LOG_CATEGORY(LogUnrealSplit);

void FUnrealSplitModule::StartupModule()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("UnrealSplit module started."));
}

void FUnrealSplitModule::ShutdownModule()
{
	UE_LOG(LogUnrealSplit, Log, TEXT("UnrealSplit module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealSplitModule, UnrealSplit)
