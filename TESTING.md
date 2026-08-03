# UnrealSplit testing protocol

Repeatable checklist for sessions, remote view, and network-condition study.
Packet simulation is **opt-in only** — nothing in this plugin or the game's
`DefaultEngine.ini` turns lag/loss on at launch. You must run `US.NetProfile`
(or pass `-ExecCmds=`) yourself.

## Prerequisites

- Editor or Development build (packet simulation is compiled out of Shipping:
  `DO_ENABLE_NET_TEST`).
- For EOS: Developer Authentication Tool running on `localhost:6300` with
  `dev1` / `dev2` (or your chosen passwords).
- Pair launchers (optional): `Scripts/run-lan-pair.ps1`,
  `Scripts/run-eos-pair.ps1` in the game repo. **Do not** pass `-NetProfile`
  unless you intend to inject conditions.

## 1. Couch (local splitscreen)

- [ ] One process, no session. `US.AddLocalPlayer` → two real locals.
- [ ] Views split; `US.ViewState` shows no shadow view.
- [ ] `US.NetProfile good` then move around — framing should feel unchanged
      (no remote `NetConnection`). Then `US.NetProfile off`.
- [ ] `US.RemoveLocalPlayer` back to one; primary removal refused when alone.

## 2. LAN pair (Null)

- [ ] `.\Scripts\run-lan-pair.ps1` (or two windows with `-ConnectionMode=Null`).
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

## 4. EOS pair

- [ ] Dev Auth Tool up. `.\Scripts\run-eos-pair.ps1` (auth args baked in).
- [ ] Same host/find/join smoke as LAN.
- [ ] Optional: one profile tier (e.g. `good` or `average`) — remember real EOS
      latency stacks with injection.
- [ ] `US.NetProfile off` before you quit so the next session is clean.

## Automated tests (session state machine)

Developer module `UnrealSplitTests`. Single process, **Null OSS only**. Never
enables packet simulation. Filter prefix: `UnrealSplit` (covers
`UnrealSplit.Session.*`).

### Run in editor

Session Frontend → Automation → filter `UnrealSplit`, or console:

```
Automation RunTests UnrealSplit
```

### Run headless

```
UnrealEditor-Cmd.exe "<path-to>/wavelengths.uproject" -ExecCmds="Automation RunTests UnrealSplit; Quit" -unattended -nopause -nullrhi -log
```

Optional HTML/JSON report directory:

```
UnrealEditor-Cmd.exe "<path-to>/wavelengths.uproject" -ExecCmds="Automation RunTests UnrealSplit; Quit" -unattended -nopause -nullrhi -log -ReportExportPath="D:/temp/us-automation"
```

Use `-ConnectionMode=Null` (or the project default) so the editor process is on
Null; EOS login is not required for this suite.

### Groups covered

| Group | Examples |
|-------|----------|
| Lifecycle | `…Lifecycle.HostSucceeds`, `HostLeaveIdle`, **`HostLeaveHostAgain`** (mandatory cycle) |
| Busy refusals | `…Busy.HostWhileHosting`, `JoinWhileHosting`, `LeaveWhileIdle`, `HostReentrancyNeverTwoSessions` |
| Capacity | `…Capacity.TwoLocalsRefuseThird`, `SlotRecycling`, `HostClampDegenerate` (`Host 0` / `Host 99`) |
| Registration | primary refuse, secondary remove, double-unregister, OSS double-register, unregister never-registered |
| Failure | `…Failure.JoinInvalidIndex`, `JoinStaleNoSearch` |

### Intentionally not automated here

- Host while joined / Join while already joined (need `InSessionAsClient` → second process)
- Capacity local+remote combined (remote seat → pair protocol above)
- CreateSession / DestroySession natural OSS failure (Null rarely fails; no runtime test seam)
- Discovery, cross-process join, view replication, ViewSlotIndex (manual sections 2–4)

If a test fails: treat it as a real session-layer bug candidate — fix is separate
from this suite (hard rule: tests report, they don’t patch runtime).

## Profiles reference

Defined in the game `Config/DefaultEngine.ini` as
`[PacketSimulationProfile.<name>]`. Wrapper: `US.NetProfile <name|off>`.
Engine equivalent: `NetEmulation.PktEmulationProfile` / `NetEmulation.Off`.
