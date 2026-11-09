# Configuration

Everything UnrealSplit needs is set in your project's config files. The [README](../README.md#minimal-setup) has the minimal setup for couch and LAN. This page covers the details and online play through Epic Online Services (EOS).

- [Plugins](#plugins)
- [Engine settings](#engine-settings)
- [Connection modes](#connection-modes)
- [LAN (Null)](#lan-null)
- [Online (EOS)](#online-eos)
- [Tunable settings](#tunable-settings)
- [Network condition profiles (optional)](#network-condition-profiles-optional)

## Plugins

| Plugin | Needed for |
|---|---|
| EnhancedInput, OnlineSubsystem, OnlineSubsystemUtils | always (enabled automatically by UnrealSplit) |
| OnlineSubsystemNull | LAN, and the default backend |
| OnlineSubsystemEOS | online play |

## Engine settings

`Config/DefaultEngine.ini`:

```ini
[/Script/Engine.Engine]
; UnrealSplit's viewport client decides where each player's image goes.
GameViewportClientClassName=/Script/UnrealSplit.USGameViewportClient

[/Script/EngineSettings.GameMapsSettings]
; The layout itself is drawn by UnrealSplit. The engine still reads these for
; safe zones, subtitle placement and HUD queries, and Vertical matches what
; UnrealSplit draws: a vertical divider.
bUseSplitscreen=True
TwoPlayerSplitscreenLayout=Vertical

[/Script/Engine.GameSession]
; The authoritative player cap: the engine checks every login against it.
MaxPlayers=2
```

You don't need a special game mode. UnrealSplit hooks into logins on its own. `AUSGameModeBase`, `AUSGameStateBase` and `AUSPlayerStateBase` are optional base classes that only set matching defaults and log joins and leaves.

## Connection modes

A process runs in exactly one mode, chosen at launch:

| Mode | Backend | Launch flag |
|---|---|---|
| LAN | OnlineSubsystemNull | `-ConnectionMode=Null` (or `LAN`) |
| Online | OnlineSubsystemEOS | `-ConnectionMode=EOS` (or `Internet`) |

- **Without the flag**, `DefaultPlatformService` in `[OnlineSubsystem]` decides. `Null` there makes LAN the default.
- **Couch co-op works in both modes.** It doesn't need a session at all.
- **Switching modes means relaunching.**
- **The net driver is handled for you.** The `UnrealSplitStartup` module rewrites the engine's `GameNetDriver` entry at startup: the IP driver for LAN, the EOS driver for online.
  - **Don't add** Epic's usual `+NetDriverDefinitions=(DefName=GameNetDriver,DriverClassName=/Script/SocketSubsystemEOS.NetDriverEOS,...)` line. It would force the EOS driver for LAN and Play In Editor as well.
- **Host and joiner must run the same build.** Sessions are filed under a key that includes the build's network version, so two different builds don't see each other's sessions.

## LAN (Null)

```ini
[OnlineSubsystem]
DefaultPlatformService=Null

[OnlineSubsystemNull]
bEnabled=true
; Gives each local user its own id. Without it, Null can hand both couch
; players the same id, and the session counts them as one player.
bAddUserNumToNullId=True
```

LAN needs no login.

## Online (EOS)

1. **Create a product** in the [Epic Developer Portal](https://dev.epicgames.com/portal) with a client for your game. Note the product, sandbox, deployment and client IDs.
2. **Enable OnlineSubsystemEOS** in your project.
3. **Add the config below** with your own IDs.

```ini
[OnlineSubsystemEOS]
bEnabled=true

[/Script/OnlineSubsystemEOS.EOSSettings]
CacheDir=CacheDir
; Must match the ArtifactName below, or EOS starts without credentials.
DefaultArtifactName=MyGame
+Artifacts=(ArtifactName="MyGame",ProductId="<product id>",SandboxId="<sandbox id>",DeploymentId="<deployment id>",ClientId="<client id>",ClientSecret="<client secret>")
; Epic Account Services: needed for login (including the Developer Authentication Tool).
bUseEAS=True
; Only ask for the scopes the game needs. The ClearArray line matters:
; config arrays merge with the engine's defaults instead of replacing them.
!AuthScopeFlags=ClearArray
+AuthScopeFlags="BasicProfile"
; Game services: sessions and P2P.
bUseEOSConnect=True
bUseEOSRTC=False
bEnableOverlay=False
bEnableSocialOverlay=False
bEnableEditorOverlay=False
```

⚠ **Keep your client secret out of public repositories.** If it was ever committed to a public repo, rotate it in the Developer Portal.

**Testing with the Developer Authentication Tool:**
1. Start the tool from the EOS SDK on `localhost:6300` and log in one account per player (for example `dev1` and `dev2`).
2. Launch each process with:
   ```
   -ConnectionMode=EOS -AUTH_TYPE=developer -AUTH_LOGIN=localhost:6300 -AUTH_PASSWORD=dev1
   ```
   Use `dev2` for the second process.

The first session call logs in automatically. `US.Login` is only a retry. Login flows for a shipped game (Epic account, Steam and so on) are up to your project and not covered here.

## Tunable settings

All of these have working defaults. Change them only if you need to.

### `DefaultEngine.ini`: `[/Script/UnrealSplit.USSessionSubsystem]`

| Setting | Default | Meaning |
|---|---|---|
| `MaxTotalPlayers` | `2` | players allowed in the game, local and remote together. An early, clearly logged check; `GameSession.MaxPlayers` stays the hard cap |
| `EntryMapOverride` | empty | map to return to when a session ends; empty = the project's default map |
| `bLanRequiresLogin` | `false` | whether LAN needs a logged-in user before session calls |
| `bInternetRequiresLogin` | `true` | whether online play needs a logged-in user before session calls |
| `bInternetUsesLobbies` | `true` | advertise online sessions as lobbies. Host and joiner must agree, or searches come back empty |

### `DefaultGame.ini`: `[/Script/UnrealSplit.USPlayerViewProxy]` (partner view stream)

| Setting | Default | Meaning |
|---|---|---|
| `SendRateHz` | `30` | camera samples sent per second |
| `InterpolationDelaySeconds` | `0.1` | how far behind the sender the partner view plays. Must exceed the network jitter, or the view stutters |
| `MaxBufferedSamples` | `32` | buffer size (about one second at 30 Hz) |
| `CutDistanceThreshold` | `500` | a camera that moves further than this (cm) in one sample is treated as a cut, not interpolated |
| `ClockCorrectionGain`, `MaxClockCorrection`, `ClockResyncThresholdSeconds`, `MaxPlaybackAdvanceSeconds`, `SampleGapResyncSeconds`, `SampleGapResyncIntervalMultiplier` | see header | fine-tuning of the playback clock; documented in `USPlayerViewProxy.h` |

### `DefaultGame.ini`: `[/Script/UnrealSplit.USRemoteViewSubsystem]`

| Setting | Default | Meaning |
|---|---|---|
| `bRemoteViewEnabled` | `true` | whether this machine draws the partner's view at all (`US.RemoteView` toggles it) |
| `ShadowPlatformUserIndex` | `7` | platform user slot for the internal player that renders the partner's view. Must not collide with a real controller |

### `DefaultGame.ini`: `[/Script/UnrealSplit.USGameViewportClient]`

| Setting | Default | Meaning |
|---|---|---|
| `ViewOrderMode` | `Fixed` | `Fixed`: player 1 (the host) is always on the left. `Own`: this machine's player is on the left |
| `AudioListenerMode` | `Auto` | `Own`: hear the level from your own view only. `Shared`: from both views, each sound played once from the nearer one. `Auto`: `Shared` in couch play, `Own` online |

## Network condition profiles (optional)

`US.NetProfile <name>` applies a named packet-simulation profile for testing under lag and loss. The profiles are ordinary engine config, so copy the ones you want into `DefaultEngine.ini`:

```ini
[PacketSimulationProfile.good]
PktLag=30
PktLagVariance=8
PktLoss=0
PktIncomingLagMin=25
PktIncomingLagMax=40
PktIncomingLoss=0

[PacketSimulationProfile.average]
PktLag=100
PktLagVariance=25
PktLoss=1
PktIncomingLagMin=80
PktIncomingLagMax=120
PktIncomingLoss=1

[PacketSimulationProfile.poor]
PktLag=200
PktLagVariance=50
PktLoss=5
PktIncomingLagMin=150
PktIncomingLagMax=250
PktIncomingLoss=5
```

The profiles:
- do nothing until you apply one
- only affect remote connections, so couch play is unchanged
- are compiled out of Shipping builds

`US.NetProfile off` clears the active one.
