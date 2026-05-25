# ARCHITECTURE — VOTV coop mod

**Living document.** Edit as understanding evolves (do not append-and-keep-
stale — that's what `research/findings/` is for).

## What this is, architecturally

A **standalone** hook-only mod that runs as an engine-extension layer on
top of UE4.27 + VOTV. It loads via our own proxy DLL (`xinput1_3.dll`
forwards XInputGetState/SetState to System32's xinput1_4.dll and
side-loads `votv-coop.dll`), discovers the game's classes/functions
through UE reflection (resolved standalone via AOB signatures — no
UE4SS at runtime), and adds a second networked player by driving the
engine's own `APawn` / `APlayerController` systems. It does not modify
any original game file (principle 1). It augments single-player; it
does not replace it (principle 6).

## Layer stack (top = closest to gameplay)

```
┌─────────────────────────────────────────────────────────────┐
│ coop/ (gameplay + network layer)                            │
│   RemotePlayer · session/peer · pose/reliable packets       │
│   interpolation · nameplate · event_feed · prop_lifecycle   │
│   prop_snapshot · npc_sync · remote_prop · grab_observer    │
├─────────────────────────────────────────────────────────────┤
│ dev/ (developer-only convenience features, ini-gated)       │
│   freecam · pos_hud · restore_vitals · teleport_client      │
├─────────────────────────────────────────────────────────────┤
│ harness/ (autonomous test driver)                           │
│   scenario timeline · autotest · sdk_check · screenshot     │
├─────────────────────────────────────────────────────────────┤
│ clean API boundary (headers in include/)                    │
├─────────────────────────────────────────────────────────────┤
│ ue_wrap/ (engine-wrapper layer)                             │
│   reflection · sig_scan · hook · game_thread · call         │
│   engine{,_pawn,_component,_widget,_bones} · puppet · prop  │
│   reflected_offset · hud_feed · log                         │
│   Reflection access, struct offsets, UFunction thunks.      │
│   NO network/gameplay/coop state.                           │
├─────────────────────────────────────────────────────────────┤
│ loader/  (xinput_proxy.cpp -> xinput1_3.dll)                │
│   Auto-load votv-coop.dll on game start.                    │
├─────────────────────────────────────────────────────────────┤
│ third_party/minhook (MIT, MinHook for the game-thread       │
│   ProcessEvent detour — static-CRT linked)                  │
├─────────────────────────────────────────────────────────────┤
│ UE4.27 + Voices of the Void (unmodified)                    │
└─────────────────────────────────────────────────────────────┘
```

The **`ue_wrap` ↔ `coop` split is principle 7.** A file that BOTH touches
engine memory/reflection AND owns network state is a violation — split it
(WP17).

## Not an ASI

This mod is **not** an ASI (the GTA/MTA-era native-DLL-via-ASI-loader
pattern from the methodology's origin). It is a runtime DLL loaded into the
UE4 process via our own `xinput1_3.dll` proxy (VOTV imports only
`XInputGetState`/`XInputSetState` from xinput1_3.dll; those are forwarded
to System32's xinput1_4.dll via `/export:` linker directives, and the
proxy's `DllMain` side-loads `votv-coop.dll`). No injection, no third-
party loader. See `src/loader/xinput_proxy.cpp` for the loader source.

## How far we can reach into the engine

A DLL in the game process can reach **everything** — there is no sandbox:

- **Reflected surface (the easy 95%)**: UE exposes every `UClass`,
  property, and `UFunction` (Blueprint *and* native) via `GUObjectArray` /
  `GNames`. We can read/write any property, call any `UFunction`, and hook
  any `UFunction` at the `ProcessEvent` level (intercept every Blueprint
  event/native call). VOTV is heavily Blueprint, so most "deep core game
  functions" ARE reflected and directly reachable.
- **Raw native surface (the other 5%)**: anything not reflected — inlined
  engine internals, native helpers, raw memory — is reachable the same way
  any native mod reaches it: AOB/signature scanning + MinHook/Detours/vtable
  hooks + direct memory patching. The IDA Pro IDB is the tool for finding
  those sites.

So "can a UE4SS mod reach deep core functions?" — yes, both layers. UE4SS
just makes the reflected layer convenient; the raw layer is always
available because we are native code in-process.

## Substrate: standalone (RULE №3 — no UE4SS at runtime)

The shipping mod **does not depend on UE4SS**. UE4SS is a development
tool only (used in the `Game_0.9.0n_dev/` copy for Live View, Lua probe
scripting, header dumps, and BP bytecode inspection — see
`docs/RE_WORKFLOW.md`).

What UE4SS used to provide vs how we provide it now:

| Capability | Shipping mod source |
|---|---|
| Injection (`dwmapi.dll` proxy) | **Our own `xinput1_3.dll` proxy** (`src/loader/xinput_proxy.cpp`) |
| Reflection access (`GUObjectArray`/`GNames` resolved) | **AOB-resolved standalone** (`ue_wrap/sig_scan.cpp` + `ue_wrap/reflection.cpp`); algorithms adapted from RE-UE4SS (MIT) with attribution |
| `UFunction` hook engine (`ProcessEvent` hook) | **MinHook detour on `ProcessEvent`** (`ue_wrap/hook.cpp` + `ue_wrap/game_thread.cpp`) — same technique UE4SS uses; static-CRT linked |
| Lua + ImGui + bundled mods | Test tooling only — `tools/probes/`, `Game_0.9.0n_dev/` |

**The discipline that makes this clean**: all engine/substrate access
lives behind `ue_wrap/`. The `coop/` gameplay-network layer never
touches reflection / GUObjectArray / sig-scan directly. The CXX header
dump (regenerated per game version) is our standalone SDK — the
class/offset/signature knowledge we need without UE4SS at runtime.

Test tooling (`tools/probes/*`, `Game_0.9.0n_dev/`) MAY depend on UE4SS
freely — it is not shipped. Only `src/votv-coop` carries the no-UE4SS
constraint, and that constraint is enforced (no UE4SS-named symbols
appear in the shipping module).

## Networking model (shipped Phase 3 — see `coop/net/`)

- **Transport**: custom UDP, pure I/O at the bottom (`coop/net/transport.cpp`).
  Host-authoritative, LAN-first.
- **Sessions, not connections**: a host listening on a port + zero/one
  client (`coop/net/session.cpp`). Per-session sequence counter +
  session-token + peer-lock; bounded drain; NaN/AABB validate; RFC1982
  sequence numbering.
- **3-layer split**: transport (bytes) → serialization (struct↔bytes) →
  application (route packets to handlers). Principle 7 applied to network.
- **Wire format is semantic** (FName string keys, vec3 positions — never
  UE memory addresses or vtable pointers; anti-pattern A7).
- **Two channels on one socket** (RULE 1 — one socket, two channels
  by reliability class, not a second transport):
  - **Unreliable pose stream**: 60 Hz `PoseSnapshot` + receiver-side
    50 ms LERP interpolation pump. Newest-wins, freely dropped.
  - **Reliable channel** (`coop/net/reliable_channel.cpp`):
    stop-and-wait ARQ + sequence space distinct from the pose stream;
    250 ms RTO. Carries: Join / Bye / Chat / RestoreVitals /
    TeleportClient / PropSpawn / PropDestroy / EntityDestroy / (future)
    DoorState / LightState.
- **Replicate authoritative state; re-derive the rest locally.** The
  receiving UE engine plays the streamed pose onto the puppet (a
  `mainPlayer_C` orphan with AutoPossess disabled) so anim, IK, weapon,
  and interaction "just work" using the engine's own systems.

## Parallel class hierarchy (principle 3)

```
coop::RemotePlayer  ──m_pEnginePawn──▶  APawn* (engine-owned)
   owns: net state, interp buffer,        owns: render, anim,
   input buffer, ownership                physics, collision
```

Mirrors MTA's `CClientPed::m_pPlayerPed -> CPlayerPed*`.

## The 7 principles (summary — full text in COOP_METHODOLOGY.md)

1. No modification of original game files.
2. Engine-extension paradigm.
3. Parallel class hierarchy mirroring engine structures.
4. Targeted crash fixes, not broad suppression.
5. Minimum viable subset (`COOP_SCOPE.md` is law).
6. Augment SP, never replace it.
7. Engine-wrapper layer vs gameplay/network layer.

## Diagrams

_TBD — add sequence diagrams for session handshake, pose-sync, and the
input-replication path as those phases land._
