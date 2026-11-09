# Console commands

Every feature of UnrealSplit can be driven from the console (`` ` ``) with the `US.` commands below. They're meant for testing and debugging. In a game, call the same functions through the subsystems (see [How it works](HowItWorks.md#public-api)).

Arguments in `<angle brackets>` are required, `[square brackets]` optional.

## Sessions

| Command | What it does |
|---|---|
| `US.Host [MaxPlayers] [Map]` | Creates a session and opens a listen server. `MaxPlayers` defaults to 2. Without a map the current level is reopened. |
| `US.Find` | Searches for joinable sessions. Run `US.SessionState` afterwards to list the results. |
| `US.Join [Index]` | Joins a session found by `US.Find` (default index 0). |
| `US.Leave` | Ends the current session and returns to the entry map. |
| `US.Travel <MapName>` | Takes everyone, all local players included, to another map and keeps the session. Host or standalone only. |
| `US.Login` | Logs the primary user into the backend. Only needed as a retry; the first session call logs in automatically, and LAN needs no login. |
| `US.SessionState` | Reports session state, connection mode and backend, this machine's local players, and the last search results. |

## Local players and input

| Command | What it does |
|---|---|
| `US.AddLocalPlayer` | Adds a couch co-op player on this machine. On a joined client, this asks the host for a split join, which needs a free slot there. |
| `US.RemoveLocalPlayer` | Removes the most recently added couch player. The primary player is never removed. |
| `US.InputDevices` | Lists every connected input device and the local player it drives, with the device IDs `US.AssignDevice` takes. |
| `US.AssignDevice <DeviceId> <PlayerIndex>` | Routes an input device to a local player. Keyboard and mouse count as one device and can be moved too. |
| `US.SwapDevices [First] [Second]` | Exchanges all input devices between two local players (default 0 and 1). |

## Screen layout (server or standalone)

These change the layout for both players and replicate to every machine.

| Command | What it does |
|---|---|
| `US.SplitRatio <share> [seconds]` | Sets the share of the screen width (0 to 1) given to player 1, with a blend (default 1 s). Doesn't leave a merged view. |
| `US.Split [share] [seconds]` | Returns both players to their own views (default 0.5 over 1 s). |
| `US.Merge [seconds] [actor]` | Merges both players onto one full-screen view: player 1's camera by default, or any actor you name (a level camera, for instance). |
| `US.ViewTargets [filter]` | Lists what `US.Merge` can merge onto, with the exact names to type and whether the remote machine can resolve them. |
| `US.SetViewSlot <PlayerIndex> <SlotIndex>` | Puts a player in a view slot (screen side). Player indices are in login order on the host (0 = host). An occupied slot is refused; use `US.SwapViewSlots`. |
| `US.SwapViewSlots [First] [Second]` | Swaps two players' sides of the screen on every machine (default 0 and 1). |

## Local presentation (this machine only)

These are never replicated, so the other machine is unaffected.

| Command | What it does |
|---|---|
| `US.ViewOrder [fixed\|own]` | `fixed`: player 1 (the host) is always on the left. `own`: this machine's player is on the left. |
| `US.AudioMode [own\|shared]` | `own`: hear the level from your own view only. `shared`: from both views, each sound played once from the nearer one. |
| `US.RemoteView [on\|off]` | Shows or hides the partner's view on this machine. Without an argument, reports the current state. |

## Diagnostics

| Command | What it does |
|---|---|
| `US.ViewState` | Prints the current layout: live and target ratio, merge state, blend progress, stream buffer, and whether this machine may change the layout. |
| `US.NetProfile [name\|off]` | Applies or clears a packet-simulation profile by the name it has in your config (for example `good`, `average`, `poor`). Opt-in, never applied automatically, and compiled out of Shipping. See [Configuration](Configuration.md#network-condition-profiles-optional). |
