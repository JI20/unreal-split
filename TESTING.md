# UnrealSplit testing protocol

Repeatable checklist for sessions, remote view, and network-condition study.
Packet simulation is **opt-in only** — nothing in this plugin or your
`DefaultEngine.ini` turns lag/loss on at launch. You must run `US.NetProfile`
(or pass `-ExecCmds=`) yourself.

## Prerequisites

- Editor or Development build (packet simulation is compiled out of Shipping:
  `DO_ENABLE_NET_TEST`).
- For EOS: Developer Authentication Tool running on `localhost:6300` with
  `dev1` / `dev2` (or your chosen passwords).
- Launching: a Standalone Game for couch play, or two game processes with
  `-ConnectionMode=Null` (LAN) or `-ConnectionMode=EOS` plus the dev-auth
  arguments (EOS), see [Docs/Configuration.md](Docs/Configuration.md). **Do not**
  apply a net profile unless you intend to inject conditions.

## 1. Couch (local splitscreen)

- [ ] One process, no session (Standalone Game).
      `US.AddLocalPlayer` → two real locals.
- [ ] Views split; `US.ViewState` shows no shadow view.
- [ ] `US.NetProfile good` then move around — framing should feel unchanged
      (no remote `NetConnection`). Then `US.NetProfile off`.
- [ ] `US.RemoveLocalPlayer` back to one; primary removal refused when alone.

## 2. LAN pair (Null)

- [ ] Two game windows with `-ConnectionMode=Null`.
- [ ] Host: `US.Host` — Client: `US.Find` → `US.Join`.
- [ ] Both machines: two view slots, Fixed order (host left / player 1 left).
- [ ] `US.ViewState` — shadow active on the one-local machine; buffer depth,
      playback offset, sample Hz look sane.
- [ ] No net profile applied yet — baseline smoothness.

## 3. Profile tiers (after a live LAN or EOS session)

Apply on **one** machine first (usually the client), then optionally both.
Always clear when done: `US.NetProfile off`.

| Step | Command | What to watch |
|------|---------|----------------|
| Clean | `US.NetProfile clean` | Mild delay on partner view; stream still smooth |
| Good | `US.NetProfile good` | Near-baseline; slight delay vs clean off |
| Average | `US.NetProfile average` | Visible lag; rare hitches; `US.ViewState` buffer/offset |
| Poor | `US.NetProfile poor` | Soft camera; more resyncs; `stat net` loss |
| Torture | `US.NetProfile torture` | Ugly on purpose; should not crash; clear after |

Also useful: `stat unit`, `stat net`, `US.ViewState`.

- [ ] Walk the ladder clean → torture (or sample good + poor).
- [ ] `US.NetProfile off` on every process you touched — confirm feel returns.

## 4. Audio listener policy (`US.AudioMode`)

Local, per-machine, never replicated. `own` = one set of ears on your own view;
`shared` = one per view, each spatialised sound played **once** from the nearer.
Global 2D sound (music, dialogue) is unaffected by either. `US.ViewState` reports
the mode and where it was resolved from; the default (`Auto`) reproduces whatever
the topology did before 1.9 — couch `shared`, remote `own`.

You need a sound that is spatialised and clearly positional: footsteps or a jump
land well, music does not.

### Couch

- [ ] Standalone Game, two pads, `US.AddLocalPlayer`.
- [ ] P2 walks far away from P1 and jumps. Default (`shared`): audible.
- [ ] `US.AudioMode own` → P2's distant footsteps go silent. Walking P2 back toward
      P1's camera brings them back.
- [ ] `US.AudioMode shared` → distant footsteps audible again.

### Remote

- [ ] LAN pair (two windows with `-ConnectionMode=Null`), host/join, partner walks far away on machine B.
- [ ] On A, default (`own`): partner's footsteps inaudible.
- [ ] `US.AudioMode shared` on **A only** → audible, from the shadow view's
      perspective (delayed and smoothed exactly like the image).
- [ ] Machine B unchanged — confirm with `US.ViewState` on B.

### Abuse

- [ ] Spam `US.AudioMode shared` / `own` repeatedly: no crash, no doubled or phasing
      sound, `US.ViewState` always matches what you hear.
- [ ] Switch during a split↔merge blend, and again mid-travel (host, leave).
- [ ] `US.AudioMode auto` is refused on purpose (it is a config-only default).

## 5. EOS pair

- [ ] Dev Auth Tool up. Two game windows with `-ConnectionMode=EOS -AUTH_TYPE=developer -AUTH_LOGIN=localhost:6300 -AUTH_PASSWORD=dev1` (`dev2` for the second).
- [ ] Same host/find/join smoke as LAN.
- [ ] Optional: one profile tier (e.g. `good` or `average`) — remember real EOS
      latency stacks with injection.
- [ ] `US.NetProfile off` before you quit so the next session is clean.

## Automated tests (session state machine, audio policy state)

Developer module `UnrealSplitTests`. Single process, **Null OSS only**. Never
enables packet simulation. Filter prefix: `UnrealSplit` (covers
`UnrealSplit.Session.*` and `UnrealSplit.Audio.*`).

**These tests need a running game world.** They drive the real session flow, and
hosting reopens the current map as a listen server — that only works in a game
world, so the tests never fabricate one. With no PIE running, every test logs
`Skipped: no PIE/game world` and passes without touching anything.

Tests must run in the **same process** as the game world. Two ways:

### A. Standalone game (preferred)

Play dropdown → **Standalone Game**, or a Development build without the
editor open at all. In the game window press `` ` `` and run:

```
Automation RunTests UnrealSplit
```

Passing `-ExecCmds="Automation RunTests UnrealSplit"` at launch does the same.
Never combine it with a net profile: the suite must not run against injected
conditions.

The editor stays out of it entirely; travel happens in the throwaway process.

### B. Play In Editor

1. Press Play (PIE), leave it running.
2. Session Frontend → Automation → filter `UnrealSplit`, run selected. Or in the
   PIE console: `Automation RunTests UnrealSplit`.

Either way, expect the world to travel during host/leave tests (listen reopen,
then back to the entry map) — that is the flow under test, not a fault.

Typing `Automation RunTests` into the **editor** console (no Play running) skips
every test with `Skipped: no PIE/game world`: the editor world has no game
instance, so there is no session subsystem to drive.

### Run headless

Needs a game world too, so run the game target (not `-ExecCmds` on a bare
editor, which would skip everything):

```
<YourGame>.exe <MapPath> -ConnectionMode=Null -ExecCmds="Automation RunTests UnrealSplit; Quit" -unattended -nopause -log -ReportExportPath="<ReportFolder>"
```

EOS login is not required for this suite; Null is enough.

### Groups covered

| Group | Examples |
|-------|----------|
| Lifecycle | `…Lifecycle.HostSucceeds`, `HostLeaveIdle`, **`HostLeaveHostAgain`** (mandatory cycle) |
| Busy refusals | `…Busy.HostWhileHosting`, `JoinWhileHosting`, `LeaveWhileIdle`, `HostReentrancyNeverTwoSessions` |
| Capacity | `…Capacity.TwoLocalsRefuseThird`, `SlotRecycling`, `HostClampDegenerate` (`Host 0` / `Host 99`) |
| Registration | primary refuse, secondary remove, double-unregister, OSS double-register, unregister never-registered |
| Failure | `…Failure.JoinInvalidIndex`, `JoinStaleNoSearch` |
| Audio policy | `UnrealSplit.Audio.Policy.*` — Auto resolution, command parsing, idempotent switching, reset to config |

`UnrealSplit.Audio.Policy.AutoResolvesPerTopology` is pure state logic and runs
anywhere; the rest need a game world like the session groups. None of them assert
on rendered audio — that is what the manual recipe in section 4 is for. They
restore whatever mode the machine was in when they started.

### Intentionally not automated here

- Host while joined / Join while already joined (need `InSessionAsClient` → second process)
- Capacity local+remote combined (remote seat → pair protocol above)
- CreateSession / DestroySession natural OSS failure (Null rarely fails; no runtime test seam)
- Discovery, cross-process join, view replication, ViewSlotIndex (manual sections 2–4)

If a test fails: treat it as a real session-layer bug candidate — fix is separate
from this suite (hard rule: tests report, they don’t patch runtime).

## Profiles reference

Defined in your project's `Config/DefaultEngine.ini` (see [Docs/Configuration.md](Docs/Configuration.md#network-condition-profiles-optional)) as
`[PacketSimulationProfile.<name>]`. Wrapper: `US.NetProfile <name|off>`.
Engine equivalent: `NetEmulation.PktEmulationProfile` / `NetEmulation.Off`.
