// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Engine/Console.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
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
	 * its output device is gone by then; those use ReportAsync instead.
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
	 * Writes into the in-game console's own scrollback, which is where a
	 * command's answer landed, so a result that arrives later shows up
	 * underneath the line that promised it instead of only in a -log window.
	 * There is no console on a dedicated server or in a shipping build, and
	 * none before the viewport exists, in which case this does nothing and the
	 * log line alongside it is the whole record.
	 */
	inline void EchoToInGameConsole(const UGameInstance* GameInstance, const FString& Message)
	{
		UGameViewportClient* const Viewport = GameInstance != nullptr ? GameInstance->GetGameViewportClient() : nullptr;

		if (UConsole* InGameConsole = Viewport != nullptr ? Viewport->ViewportConsole : nullptr)
		{
			InGameConsole->OutputText(Message);
		}
	}

	/** Reports an outcome that resolved after the command that started it returned. */
	inline void ReportAsync(const UGameInstance* GameInstance, const FString& Message)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("%s"), *Message);
		EchoToInGameConsole(GameInstance, Message);
	}

	inline void ReportAsyncWarning(const UGameInstance* GameInstance, const FString& Message)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("%s"), *Message);
		EchoToInGameConsole(GameInstance, Message);
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
