# ARCHITECTURE — VOTV coop mod

**Living document.** Edit as understanding evolves (do not append-and-keep-
stale — that's what `research/findings/` is for).

## What this is, architecturally

A hook-only mod that runs as an engine-extension layer on top of UE4.27 +
VOTV, on its **own substrate**. It ships as a standard UE4SS mod folder
(`Mods\Multivoid\dlls\main.dll` + `enabled.txt`; UE4SS — or r2modman's
unreal-shimloader — LoadLibrary's it and starts it via the C-ABI
`start_mod()` contract, `src/loader/cppmod_entry.cpp`), but UE4SS is the
*loader only*: the DLL imports nothing from it (the D-3 slim contract).
The mod discovers the game's classes/functions through UE reflection
(resolved by its own AOB signatures), and adds a second networked player
by driving the engine's own `APawn` / `APlayerController` systems. It
does not modify any original game file (principle 1). It augments
single-player; it does not replace it (principle 6). (The previous
standalone `xinput1_3.dll` proxy loader retired whole at UE4SS_ARC WP-2
commit 3.)

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
│ loader/  (cppmod_entry.cpp + cppmod_stubs.asm)              │
│   The UE4SS C-ABI start_mod() contract + predecessor scan.  │
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
pattern from the methodology's origin). It is a runtime DLL loaded into
the UE4 process by UE4SS's mod scan (`Mods\Multivoid\dlls\main.dll`,
started via the exported `start_mod()` — `src/loader/cppmod_entry.cpp`,
which also carries the predecessor scan that refuses to start beside a
leftover pre-mod-folder install). No injection; the loader is the standard
one the game's mod ecosystem already uses, and the mod imports nothing
from it.

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

## Substrate: our own (RULE №3 — UE4SS is the loader, never the engine layer)

The mod runs *under* UE4SS but **depends on none of its machinery** — the
D-3 slim contract (decision 2026-08-21, shipped at WP-2): UE4SS
LoadLibrary's `main.dll` and calls the two exported C-ABI functions
(`start_mod` / `uninstall_mod`); everything past that line is Multivoid's
own code, and the DLL imports **zero** symbols from `UE4SS.dll`
(machine-checked by `tools/loader/abi_gate.py`, run per-push in CI on the
built artifact with its own must-fire drill since 2026-08-29; field UE4SS
C++ mods import 32-130 mangled symbols and break across UE4SS versions —
ours loads on 3.0.1 and the experimental build alike). UE4SS is also the
everyday development tool (Live View, Lua probes, header dumps, BP
bytecode inspection — see `docs/RE_WORKFLOW.md`).

What UE4SS could provide vs what the mod uses instead:

| Capability | Shipping mod source |
|---|---|
| Reflection access (`GUObjectArray`/`GNames` resolved) | **AOB-resolved, our own** (`ue_wrap/sig_scan.cpp` + `ue_wrap/reflection.cpp`); algorithms adapted from RE-UE4SS (MIT) with attribution |
| `UFunction` hook engine (`ProcessEvent` hook) | **MinHook detour on `ProcessEvent`** (`ue_wrap/hook.cpp` + `ue_wrap/pe_detour.cpp`) — with a followJmp-immune relay so it COMPOSES with UE4SS's own PolyHook PE detour (UE4SS_ARC §4) |
| UE4SS's Lua / C++ mod API / its ImGui | **Not used.** Our own vendored ImGui; the C++ API is the ABI-fragility the slim contract exists to avoid |

**The discipline that makes this clean**: all engine/substrate access
lives behind `ue_wrap/`. The `coop/` gameplay-network layer never
touches reflection / GUObjectArray / sig-scan directly. The CXX header
dump (regenerated per game version) is our own SDK — the
class/offset/signature knowledge we need without UE4SS's machinery.

The one deliberate exception to "no UE4SS-named symbols" is the loading
contract itself: `src/loader/cppmod_entry.cpp` exports what UE4SS's mod
scan calls, and imports nothing back.

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
