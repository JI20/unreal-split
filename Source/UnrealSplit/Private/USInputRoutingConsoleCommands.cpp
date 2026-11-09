// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "CoreMinimal.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include "USConsoleReport.h"
#include "USInputRoutingSubsystem.h"
#include "USShadowLocalPlayer.h"

// Development-only driver for input routing until the game's device-selection
// menu exists. The console is compiled out of shipping builds, so these go
// with it.
#if !UE_BUILD_SHIPPING

using USConsole::Report;
using USConsole::ReportWarning;

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSInputDevicesCommand(
	TEXT("US.InputDevices"),
	TEXT("UnrealSplit: list every connected input device and the local player it drives. The device ids listed here are what US.AssignDevice takes."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
	{
		const UUSInputRoutingSubsystem* InputRouting = USConsole::FindSubsystem<UUSInputRoutingSubsystem>(World, Ar);
		if (InputRouting == nullptr)
		{
			return;
		}

		const TArray<FUSInputDeviceInfo> Devices = InputRouting->GetInputDevices();
		Report(Ar, FString::Printf(TEXT("US.InputDevices: %d connected device(s), %d local player(s)."),
			Devices.Num(), UUSShadowLocalPlayer::CountRealLocalPlayers(World->GetGameInstance())));

		for (const FUSInputDeviceInfo& Device : Devices)
		{
			// An unassigned device is the normal state for a pad whose seat has not
			// been filled yet, so it is reported rather than treated as a problem.
			Report(Ar, FString::Printf(TEXT("  Device=%d Name='%s' PlatformUser=%d Player=%s"),
				Device.DeviceId,
				Device.DeviceName.IsEmpty() ? TEXT("unknown") : *Device.DeviceName,
				Device.PlatformUserId,
				Device.PlayerIndex != INDEX_NONE ? *FString::FromInt(Device.PlayerIndex) : TEXT("none")));
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSAssignDeviceCommand(
	TEXT("US.AssignDevice"),
	TEXT("UnrealSplit: route an input device to a local player. Usage: US.AssignDevice <DeviceId> <PlayerIndex>. Ids come from US.InputDevices; keyboard and mouse count as a device and can be moved too."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSInputRoutingSubsystem* InputRouting = USConsole::FindSubsystem<UUSInputRoutingSubsystem>(World, Ar);
		if (InputRouting == nullptr)
		{
			return;
		}

		if (Args.Num() < 2)
		{
			ReportWarning(Ar, TEXT("US.AssignDevice: needs both arguments. Usage: US.AssignDevice <DeviceId> <PlayerIndex>. Run US.InputDevices to see the device ids."));
			return;
		}

		const int32 DeviceId = FCString::Atoi(*Args[0]);
		const int32 PlayerIndex = FCString::Atoi(*Args[1]);

		if (InputRouting->AssignInputDeviceToPlayer(DeviceId, PlayerIndex))
		{
			Report(Ar, FString::Printf(TEXT("US.AssignDevice: input device %d now drives local player %d."), DeviceId, PlayerIndex));
		}
		else
		{
			// The subsystem's own line says which refusal it was.
			ReportWarning(Ar, FString::Printf(TEXT("US.AssignDevice: could not give input device %d to local player %d."), DeviceId, PlayerIndex));
		}
	}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GUSSwapDevicesCommand(
	TEXT("US.SwapDevices"),
	TEXT("UnrealSplit: exchange all input devices between two local players. Usage: US.SwapDevices [FirstPlayer] [SecondPlayer] (defaults to 0 and 1)."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		UUSInputRoutingSubsystem* InputRouting = USConsole::FindSubsystem<UUSInputRoutingSubsystem>(World, Ar);
		if (InputRouting == nullptr)
		{
			return;
		}

		const int32 FirstPlayerIndex = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
		const int32 SecondPlayerIndex = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 1;

		if (InputRouting->SwapPlayerInputDevices(FirstPlayerIndex, SecondPlayerIndex))
		{
			Report(Ar, FString::Printf(TEXT("US.SwapDevices: swapped the devices of local players %d and %d."), FirstPlayerIndex, SecondPlayerIndex));
		}
		else
		{
			ReportWarning(Ar, FString::Printf(TEXT("US.SwapDevices: could not swap the devices of local players %d and %d."), FirstPlayerIndex, SecondPlayerIndex));
		}
	}));

#endif // !UE_BUILD_SHIPPING
