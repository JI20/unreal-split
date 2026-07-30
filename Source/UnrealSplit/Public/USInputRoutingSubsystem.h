// Copyright (c) Jonas Itt. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "USInputRoutingSubsystem.generated.h"

class ULocalPlayer;

/**
 * One input device on this machine, as a device-selection menu needs to see it.
 *
 * Free of platform input types so UI and Blueprint can list devices and assign
 * them without touching the device mapper.
 */
USTRUCT(BlueprintType)
struct FUSInputDeviceInfo
{
	GENERATED_BODY()

	/** Stable handle for this device; pass it to AssignInputDeviceToPlayer. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Input")
	int32 DeviceId = INDEX_NONE;

	/**
	 * What the device reports itself as, e.g. "KBM" for keyboard and mouse or
	 * "XInputController" for an Xbox pad. Suitable as a menu label; empty when
	 * the platform has not published hardware info for the device yet.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Input")
	FString DeviceName;

	/** Platform user the device currently belongs to. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Input")
	int32 PlatformUserId = INDEX_NONE;

	/**
	 * Local player this device currently drives, or INDEX_NONE when its platform
	 * user has no local player — an unassigned pad, or one paired with a seat
	 * that has not been filled by AddLocalCoopPlayer yet.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealSplit|Input")
	int32 PlayerIndex = INDEX_NONE;
};

/** Fired whenever the device list or its player assignments change. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUSOnInputDevicesChanged);

/**
 * Input device routing for UnrealSplit: which physical device drives which
 * local player on this machine.
 *
 * Purely local state, and deliberately so. A device belongs to a platform user,
 * a local player owns a platform user, and the engine routes each input event to
 * the local player matching the event's device. Reassigning therefore means
 * moving the *device* to another platform user, never changing a local player's
 * platform user: that user is also the player's online identity, so moving it
 * would silently re-point the session login it was registered with.
 *
 * Nothing here is replicated or has any authority side. Each machine routes its
 * own hardware, and the server only ever sees players, not the devices behind
 * them — so the same assignment code covers standalone splitscreen and both
 * network modes, and two machines with one player each never need it at all.
 *
 * Its one real use beyond player preference is testing network modes on a single
 * machine: two game processes each enumerate every pad and each hands its first
 * one to player 1, so both windows end up driven by the same pad until one of
 * them is told otherwise.
 *
 * Separate from UUSSessionSubsystem because device pairing is not session
 * membership: it applies before a session exists, survives every session, and
 * never touches the online backend.
 */
UCLASS()
class UNREALSPLIT_API UUSInputRoutingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/**
	 * Every connected input device and the local player it drives, in device id
	 * order. Keyboard and mouse are one device like any other and can be assigned.
	 */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Input")
	TArray<FUSInputDeviceInfo> GetInputDevices() const;

	/** The devices currently driving one local player. */
	UFUNCTION(BlueprintPure, Category = "UnrealSplit|Input")
	TArray<FUSInputDeviceInfo> GetInputDevicesForPlayer(int32 PlayerIndex) const;

	/**
	 * Routes one input device to a local player, taking effect on the next input
	 * event; no player controller or input mapping context has to be rebuilt.
	 *
	 * The assignment lasts until the platform re-pairs the device, which it does
	 * when the device reconnects — so a pad that goes to sleep comes back on its
	 * default user, and a menu that cares should reapply on the change delegate.
	 *
	 * @param DeviceId Device from GetInputDevices().
	 * @param PlayerIndex Local player to route it to, 0-based.
	 * @return Whether the device now belongs to that player, including when it
	 *   already did.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Input")
	bool AssignInputDeviceToPlayer(int32 DeviceId, int32 PlayerIndex);

	/**
	 * Exchanges every device between two local players, the usual "swap
	 * controllers" menu action. All-or-nothing: a failed exchange is rolled back
	 * so neither player is left holding both sets.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealSplit|Input")
	bool SwapPlayerInputDevices(int32 FirstPlayerIndex, int32 SecondPlayerIndex);

	/**
	 * Fired when a device connects or disconnects and when an assignment changes,
	 * including assignments the platform makes on its own. Menus should re-read
	 * GetInputDevices() rather than caching the list.
	 */
	UPROPERTY(BlueprintAssignable, Category = "UnrealSplit|Input")
	FUSOnInputDevicesChanged OnInputDevicesChangedDelegate;

private:
	/** Fills in the local player and hardware name behind a device id. */
	FUSInputDeviceInfo DescribeDevice(FInputDeviceId DeviceId) const;

	/** Local player owning a platform user, or nullptr when no seat holds it. */
	const ULocalPlayer* FindPlayerForPlatformUser(FPlatformUserId UserId) const;

	void HandleDeviceConnectionChanged(EInputDeviceConnectionState NewState, FPlatformUserId UserId, FInputDeviceId DeviceId);
	void HandleDevicePairingChanged(FInputDeviceId DeviceId, FPlatformUserId NewUserId, FPlatformUserId OldUserId);

	FDelegateHandle DeviceConnectionChangeHandle;
	FDelegateHandle DevicePairingChangeHandle;
};
