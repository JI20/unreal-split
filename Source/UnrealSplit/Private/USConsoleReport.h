// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

#include "UnrealSplitModule.h"

/**
 * Shared plumbing for the plugin's development console commands.
 *
 * Every US.* command answers the same way, so the reasoning behind it lives in
 * one place rather than in each command file.
 */
namespace USConsole
{
	/**
	 * Answers through the command's output device *and* the log. The in-game
	 * console only echoes log lines that pass con.MinLogVerbosity — nothing, by
	 * default — so without the output device these answers are invisible unless
	 * the game was launched with -log, while the log is what survives the
	 * session. Asynchronous outcomes arrive long after the command returned and
	 * cannot use this; they stay log-only.
	 */
	inline void Report(FOutputDevice& Ar, const FString& Message)
	{
		Ar.Log(Message);
		UE_LOG(LogUnrealSplit, Log, TEXT("%s"), *Message);
	}

	inline void ReportWarning(FOutputDevice& Ar, const FString& Message)
	{
		Ar.Log(ELogVerbosity::Warning, Message);
		UE_LOG(LogUnrealSplit, Warning, TEXT("%s"), *Message);
	}

	/**
	 * Resolves one of the plugin's game-instance subsystems for a console
	 * command, reporting the reason when there is none: commands are registered
	 * process-wide, so they are reachable from the editor with no game running.
	 */
	template <typename SubsystemType>
	SubsystemType* FindSubsystem(UWorld* World, FOutputDevice& Ar)
	{
		UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		SubsystemType* Subsystem = GameInstance != nullptr ? GameInstance->GetSubsystem<SubsystemType>() : nullptr;

		if (Subsystem == nullptr)
		{
			ReportWarning(Ar, TEXT("No UnrealSplit subsystem in this world. Run this from a running game or PIE session."));
		}

		return Subsystem;
	}
}
