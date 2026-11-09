# How it works

UnrealSplit describes a co-op session along two independent axes:

1. **Session mode:** local split screen or networked, owned by `UUSSessionSubsystem`.
2. **Screen layout:** a split ratio or one merged view, owned by `UUSViewSubsystem`.

What each camera looks at stays your game's job. UnrealSplit decides only *where* each view goes on the screen and *how* the partner's view reaches the other machine.

UnrealSplit also doesn't replicate your gameplay. Characters, puzzles and world state use standard Unreal replication in your game, exactly as without the plugin.

## Couch: two players, one machine

Both players are real local players. `UUSGameViewportClient` replaces the engine's fixed split-screen layouts (halves, thirds, quarters) with a vertical divider at any position, and animates it when the layout changes.

## Online: the partner's view without video

Online, each machine simulates the world but would normally render only its own player. UnrealSplit fills the second half of the screen with the partner's camera:

```
Machine A (player 1)                       Machine B (player 2)
────────────────────                       ────────────────────
own camera ──sample 30 Hz──► AUSPlayerViewProxy ──replicates──► buffer, play back 100 ms behind
                                                                  │
                                                                  ▼
                                    AUSSpectatorPlayerController → UUSShadowLocalPlayer
                                                                  │
                                                                  ▼
                                               second half of B's screen, rendered locally
```

- **`AUSPlayerViewProxy`:** one per player, spawned by the server when the player logs in and owned by that player's controller.
  - **On the player's own machine,** it samples the *finished* camera from `APlayerCameraManager`, after spring-arm collision and everything else your camera does. Each sample (`FUSViewSample`) holds the location, the rotation and field of view compressed to 16 bits each, a send time, and a cut counter.
  - **On the other machine,** it buffers the samples (up to 32) and plays them back about 100 ms behind the sender. A small clock correction keeps the delay steady. Jumps larger than `CutDistanceThreshold`, or flagged cuts, snap instead of being interpolated.
- **`UUSShadowLocalPlayer`:** a local player that exists only because Unreal's renderer gives viewports to local players. It has no input device (it sits on platform user 7), and UnrealSplit keeps it out of everything that counts players: sessions, logins and input routing.
- **`AUSSpectatorPlayerController`:** drives the shadow player's view from the stream. It exists only on that machine, is never replicated and controls nothing.

The partner's view therefore costs only a few bytes per sample. Because it's rendered locally from the real world state, it shows exactly what the partner sees, about 100 ms later.

## Layout: server-authoritative

- **One replicated actor carries the layout.** `AUSViewState`, spawned once per world on the server, holds a `FUSViewIntent`: player 1's share of the width, whether views are merged, the merge target, blend time and easing.
- **Every machine blends on its own.** Clients receive the intent and animate towards it with the same curve, so both screens end up identical without sending animation frames.
- **Only the server changes it.** Split, merge and ratio changes come from server-side logic such as level volumes or puzzle code. There is no client request path.
- **`SetSplitRatioLocal` is the exception:** an immediate, local-only ratio for effects that only one machine should see.
- **View slots** decide which side of the screen each player is on. You can attach labels to them (for example character names) for menus.

## Local presentation

Some choices belong to each machine and are never replicated:
- the view order (`Fixed`: player 1 on the left, or `Own`: me on the left)
- the audio listener policy (one listener on your own view, or one per view)
- whether the partner's view is drawn at all

## Input routing

`UUSInputRoutingSubsystem` maps physical devices (gamepads, keyboard and mouse) to local players on this machine. Assignments take effect on the next input event, without rebuilding controllers or input mappings. You can also swap two players' devices in one step, or capture the next button press on a device to give it to a player.

## Sessions

`UUSSessionSubsystem` lives on the game instance, so it survives level travel. It runs a small state machine:

`Idle → Creating → Hosting` · `Idle → Searching → Joining → InSessionAsClient` · `→ Leaving → Idle`

Every public call is refused when the current state doesn't allow it, so no two session operations ever overlap.

- **Hosting** creates an advertised session and opens a listen server.
- **Capacity checks** count local and remote players together (two in total).
- **Travel** moves everyone, local players included, and keeps the session.
- **Find list:** the host can publish a small snapshot (lobby or in progress, which slots are free, with their labels), so a search result can show it without joining.

## Startup

The `UnrealSplitStartup` module loads before the engine reads its online settings. It reads `-ConnectionMode=`, sets the online backend (Null or EOS), and points the engine's game net driver at the matching driver: IP for LAN, EOS for online. Without the flag, your config stays as it is.

## Public API

All subsystems are game-instance subsystems and callable from C++ and Blueprint.

| Class | Main functions | Events |
|---|---|---|
| `UUSSessionSubsystem` | `HostSession`, `FindSessions`, `GetFoundSessions`, `JoinFoundSession`, `LeaveSession`, `TravelSessionToMap`, `AddLocalCoopPlayer`, `RemoveLocalCoopPlayer`, `GetSessionState`, `GetConnectionMode`, `GetBackendStatus`, `LoginToBackend`, `SetAdvertisedSessionState` | `OnSessionHostedDelegate`, `OnSessionsFoundDelegate`, `OnJoinCompletedDelegate`, `OnSessionEndedDelegate`, `OnBackendReadyDelegate` |
| `UUSViewSubsystem` | `SetSplitRatio`, `SplitViews`, `MergeViews`, `SetSplitRatioLocal`, `GetViewIntent`, `GetCurrentSplitRatio`, `GetBlendProgress`, `IsViewAuthority` | — |
| `UUSRemoteViewSubsystem` | `SetRemoteViewEnabled`, `GetPlayerViewSlot`, `SetPlayerViewSlot`, `SwapPlayerViewSlots`, `SetViewSlotLabel`, `GetViewSlotLabel` | `OnViewSlotsChanged` |
| `UUSInputRoutingSubsystem` | `GetInputDevices`, `AssignInputDeviceToPlayer`, `UnassignInputDevice`, `SwapPlayerInputDevices`, `BeginDeviceCapture`, `CancelDeviceCapture` | `OnDeviceCaptureFinished`, `OnInputDevicesChangedDelegate` |
| `UUSGameViewportClient` | `SetViewOrderMode`, `SetAudioListenerMode` | — |

Every function is documented in its header under `Source/UnrealSplit/Public/`.
