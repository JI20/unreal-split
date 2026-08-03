// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USInputRoutingSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/InputDeviceSubsystem.h"
#include "GameFramework/InputSettings.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"

#include "UnrealSplitModule.h"
#include "USShadowLocalPlayer.h"

void UUSInputRoutingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Both platform-side changes matter to a device menu: a pad connecting adds a
	// row, and the platform re-pairing one on reconnect silently undoes whatever
	// assignment was made here.
	IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	DeviceConnectionChangeHandle = Mapper.GetOnInputDeviceConnectionChange().AddUObject(this, &UUSInputRoutingSubsystem::HandleDeviceConnectionChanged);
	DevicePairingChangeHandle = Mapper.GetOnInputDevicePairingChange().AddUObject(this, &UUSInputRoutingSubsystem::HandleDevicePairingChanged);
}

void UUSInputRoutingSubsystem::Deinitialize()
{
	IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	Mapper.GetOnInputDeviceConnectionChange().Remove(DeviceConnectionChangeHandle);
	Mapper.GetOnInputDevicePairingChange().Remove(DevicePairingChangeHandle);
	DeviceConnectionChangeHandle.Reset();
	DevicePairingChangeHandle.Reset();

	Super::Deinitialize();
}

TArray<FUSInputDeviceInfo> UUSInputRoutingSubsystem::GetInputDevices() const
{
	TArray<FInputDeviceId> Devices;
	IPlatformInputDeviceMapper::Get().GetAllConnectedInputDevices(Devices);
	Devices.Sort();

	TArray<FUSInputDeviceInfo> Infos;
	Infos.Reserve(Devices.Num());
	for (const FInputDeviceId DeviceId : Devices)
	{
		Infos.Add(DescribeDevice(DeviceId));
	}

	return Infos;
}

TArray<FUSInputDeviceInfo> UUSInputRoutingSubsystem::GetInputDevicesForPlayer(int32 PlayerIndex) const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const ULocalPlayer* Player = UUSShadowLocalPlayer::GetRealLocalPlayerByIndex(GameInstance, PlayerIndex);

	TArray<FUSInputDeviceInfo> Infos;
	if (Player == nullptr)
	{
		return Infos;
	}

	TArray<FInputDeviceId> Devices;
	IPlatformInputDeviceMapper::Get().GetAllConnectedInputDevicesForUser(Player->GetPlatformUserId(), Devices);
	Devices.Sort();

	Infos.Reserve(Devices.Num());
	for (const FInputDeviceId DeviceId : Devices)
	{
		Infos.Add(DescribeDevice(DeviceId));
	}

	return Infos;
}

bool UUSInputRoutingSubsystem::AssignInputDeviceToPlayer(int32 DeviceId, int32 PlayerIndex)
{
	const UGameInstance* GameInstance = GetGameInstance();

	// Player indices here count people, not views: a shadow view rendering the
	// remote partner is not addressable by any of these calls, which is what makes
	// it impossible to hand a device to the one player that must never have one.
	const ULocalPlayer* TargetPlayer = UUSShadowLocalPlayer::GetRealLocalPlayerByIndex(GameInstance, PlayerIndex);
	if (TargetPlayer == nullptr)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("AssignInputDeviceToPlayer rejected: no local player %d on this machine (%d present). Add the player first."),
			PlayerIndex, UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance));
		return false;
	}

	const FPlatformUserId NewUserId = TargetPlayer->GetPlatformUserId();
	if (!NewUserId.IsValid())
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("AssignInputDeviceToPlayer failed: local player %d has no platform user."), PlayerIndex);
		return false;
	}

	IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	const FInputDeviceId Device = FInputDeviceId::CreateFromInternalId(DeviceId);

	// A device the mapper does not know has no owner to move it away from, and
	// mapping it here would invent one the platform never registered.
	const FPlatformUserId OldUserId = Mapper.GetUserForInputDevice(Device);
	if (!OldUserId.IsValid())
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("AssignInputDeviceToPlayer rejected: input device %d is not mapped on this machine. Use US.InputDevices to list the devices that are."), DeviceId);
		return false;
	}

	if (OldUserId == NewUserId)
	{
		UE_LOG(LogUnrealSplit, Log, TEXT("Input device %d already drives local player %d."), DeviceId, PlayerIndex);
		return true;
	}

	if (!Mapper.Internal_ChangeInputDeviceUserMapping(Device, NewUserId, OldUserId))
	{
		UE_LOG(LogUnrealSplit, Error, TEXT("AssignInputDeviceToPlayer failed: the platform refused to move input device %d from platform user %d to %d."),
			DeviceId, OldUserId.GetInternalId(), NewUserId.GetInternalId());
		return false;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Input device %d ('%s') now drives local player %d (platform user %d, was %d)."),
		DeviceId, *DescribeDevice(Device).DeviceName, PlayerIndex, NewUserId.GetInternalId(), OldUserId.GetInternalId());

	// Taking a player's last device leaves it in the game with nothing to control
	// it, which looks exactly like a broken pad from the couch.
	if (const ULocalPlayer* PreviousPlayer = FindPlayerForPlatformUser(OldUserId))
	{
		TArray<FInputDeviceId> RemainingDevices;
		Mapper.GetAllConnectedInputDevicesForUser(OldUserId, RemainingDevices);
		if (RemainingDevices.IsEmpty())
		{
			UE_LOG(LogUnrealSplit, Warning, TEXT("Local player %d has no input device left after the move."), UUSShadowLocalPlayer::GetRealLocalPlayerIndex(GameInstance, PreviousPlayer));
		}
	}

	return true;
}

bool UUSInputRoutingSubsystem::SwapPlayerInputDevices(int32 FirstPlayerIndex, int32 SecondPlayerIndex)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const ULocalPlayer* FirstPlayer = UUSShadowLocalPlayer::GetRealLocalPlayerByIndex(GameInstance, FirstPlayerIndex);
	const ULocalPlayer* SecondPlayer = UUSShadowLocalPlayer::GetRealLocalPlayerByIndex(GameInstance, SecondPlayerIndex);
	if (FirstPlayer == nullptr || SecondPlayer == nullptr || FirstPlayer == SecondPlayer)
	{
		UE_LOG(LogUnrealSplit, Warning, TEXT("SwapPlayerInputDevices rejected: local players %d and %d are not two different players on this machine (%d present)."),
			FirstPlayerIndex, SecondPlayerIndex, UUSShadowLocalPlayer::CountRealLocalPlayers(GameInstance));
		return false;
	}

	IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId FirstUserId = FirstPlayer->GetPlatformUserId();
	const FPlatformUserId SecondUserId = SecondPlayer->GetPlatformUserId();

	// Both sides are read before anything moves: after the first move the mapper
	// would report the moved devices as belonging to the other player.
	TArray<FInputDeviceId> FirstDevices;
	TArray<FInputDeviceId> SecondDevices;
	Mapper.GetAllConnectedInputDevicesForUser(FirstUserId, FirstDevices);
	Mapper.GetAllConnectedInputDevicesForUser(SecondUserId, SecondDevices);

	TArray<TPair<FInputDeviceId, FPlatformUserId>> Moved;
	Moved.Reserve(FirstDevices.Num() + SecondDevices.Num());

	auto MoveDevices = [&Mapper, &Moved](const TArray<FInputDeviceId>& Devices, FPlatformUserId FromUserId, FPlatformUserId ToUserId) -> bool
	{
		for (const FInputDeviceId Device : Devices)
		{
			if (!Mapper.Internal_ChangeInputDeviceUserMapping(Device, ToUserId, FromUserId))
			{
				return false;
			}

			Moved.Emplace(Device, FromUserId);
		}

		return true;
	};

	if (!MoveDevices(FirstDevices, FirstUserId, SecondUserId) || !MoveDevices(SecondDevices, SecondUserId, FirstUserId))
	{
		// A half-finished swap is worse than none: one player would hold both sets.
		for (int32 Index = Moved.Num() - 1; Index >= 0; --Index)
		{
			const FPlatformUserId CurrentUserId = Mapper.GetUserForInputDevice(Moved[Index].Key);
			Mapper.Internal_ChangeInputDeviceUserMapping(Moved[Index].Key, Moved[Index].Value, CurrentUserId);
		}

		UE_LOG(LogUnrealSplit, Error, TEXT("SwapPlayerInputDevices failed: the platform refused a move; the original assignment was restored."));
		return false;
	}

	UE_LOG(LogUnrealSplit, Log, TEXT("Swapped input devices between local players %d and %d: %d device(s) and %d device(s)."),
		FirstPlayerIndex, SecondPlayerIndex, FirstDevices.Num(), SecondDevices.Num());
	return true;
}

FUSInputDeviceInfo UUSInputRoutingSubsystem::DescribeDevice(FInputDeviceId DeviceId) const
{
	const IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId UserId = Mapper.GetUserForInputDevice(DeviceId);

	FUSInputDeviceInfo Info;
	Info.DeviceId = DeviceId.GetId();
	Info.PlatformUserId = UserId.GetInternalId();

	if (const ULocalPlayer* Player = FindPlayerForPlatformUser(UserId))
	{
		Info.PlayerIndex = UUSShadowLocalPlayer::GetRealLocalPlayerIndex(GetGameInstance(), Player);
	}

	// The hardware name is published asynchronously by the platform, so a device
	// that has just connected can legitimately have none yet.
	if (const UInputDeviceSubsystem* InputDevices = GEngine != nullptr ? GEngine->GetEngineSubsystem<UInputDeviceSubsystem>() : nullptr)
	{
		const FHardwareDeviceIdentifier Hardware = InputDevices->GetInputDeviceHardwareIdentifier(DeviceId);
		if (Hardware.IsValid())
		{
			Info.DeviceName = Hardware.HardwareDeviceIdentifier.ToString();
		}
	}

	return Info;
}

const ULocalPlayer* UUSInputRoutingSubsystem::FindPlayerForPlatformUser(FPlatformUserId UserId) const
{
	const UGameInstance* GameInstance = GetGameInstance();
	if (GameInstance == nullptr || !UserId.IsValid())
	{
		return nullptr;
	}

	// Shadow views are skipped rather than matched. The engine's own device routing
	// matches local players by platform user, which is why a shadow view is given
	// an id above the splitscreen cap that no device answers to — and why this,
	// the plugin's equivalent lookup, must not hand a device's owner back as a
	// player either.
	for (const ULocalPlayer* Player : GameInstance->GetLocalPlayers())
	{
		if (Player != nullptr && !UUSShadowLocalPlayer::IsShadow(Player) && Player->GetPlatformUserId() == UserId)
		{
			return Player;
		}
	}

	return nullptr;
}

void UUSInputRoutingSubsystem::HandleDeviceConnectionChanged(EInputDeviceConnectionState NewState, FPlatformUserId UserId, FInputDeviceId DeviceId)
{
	UE_LOG(LogUnrealSplit, Verbose, TEXT("Input device %d connection state is now %d (platform user %d)."),
		DeviceId.GetId(), static_cast<int32>(NewState), UserId.GetInternalId());

	OnInputDevicesChangedDelegate.Broadcast();
}

void UUSInputRoutingSubsystem::HandleDevicePairingChanged(FInputDeviceId DeviceId, FPlatformUserId NewUserId, FPlatformUserId OldUserId)
{
	UE_LOG(LogUnrealSplit, Verbose, TEXT("Input device %d moved from platform user %d to %d."),
		DeviceId.GetId(), OldUserId.GetInternalId(), NewUserId.GetInternalId());

	OnInputDevicesChangedDelegate.Broadcast();
}
