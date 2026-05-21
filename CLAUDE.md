# Project rules for Claude — VOTV_MP (Voices of the Void coop mod)

This project adds functional multiplayer / coop to **Voices of the Void**
(VOTV), a single-player **Unreal Engine 4.27** game, **without modifying
any original game files**.

For any coop / multiplayer architecture decision, consult
`docs/COOP_METHODOLOGY.md` AND `reference/mtasa-blue/` (conceptual
precedent) AND `reference/RE-UE4SS/` (the UE4 modding substrate we build
on). After any non-trivial coop work, spawn an MTA-fidelity audit agent
(WP2 in the methodology).

---

## RULE №1 — No crutches, no quick fixes

Always pick the proper root-cause fix. No "good enough", no "we can fix
later", no shortcuts. Weeks or months of work to do it properly is OK.

When you find yourself adding a workaround (filter, skip-if, suppress-X,
catch-and-ignore), **STOP**. That is a crutch. Identify the root cause and
fix it at the point where the actual problem is, not broadly.

## RULE №2 — No migration baggage

When a feature is replaced or retired, the old code goes. Fully,
immediately. No "deprecated but kept for now", no feature flags for old
behaviour, no parallel old + new code paths.

Triggers: `// deprecated, kept for now` comments; flags that re-enable old
behaviour; two implementations of one concept compiled together; type
aliases kept "for compatibility"; stub functions for old callers.

---

## The 7 architectural principles

1. **No modification of original game files.** Hooks, UFunction hooks,
   reflection-driven patches, runtime memory patches — yes. Editing the
   `.exe`, the `.pak`, cooked assets, or repacking — **no**. Editing a
   blueprint asset to add replication counts as editing assets (anti-
   pattern A6); route around it.
2. **Engine-extension paradigm.** Treat the mod as a new engine layer on
   top of UE4 + VOTV, not "a few hooks". Modules own their lifecycles;
   clean APIs between subsystems.
3. **Parallel class hierarchy mirroring engine structures.** Our code owns
   a `RemotePlayer` (network state, interpolation buffer, input buffer,
   ownership of the engine entity pointer). The **engine** `APawn` /
   `ACharacter` / `APlayerController` owns rendering, animation, physics
   — keep it. Connected by a pointer. (MTA shape:
   `CClientPed::m_pPlayerPed -> CPlayerPed*`.)
4. **Targeted crash fixes, not broad suppression.** Find the specific call
   site; patch THAT site. Never a broad "filter all our orphans"
   mechanism. Transitional-crutch exception only with a written
   retirement plan (each crash masked, the targeted fix, gating criteria).
5. **Minimum viable subset.** `docs/COOP_SCOPE.md` is law. Anything not
   listed there is NOT replicated. Living document; amend with audit trail.
6. **Augment SP, never replace it.** Coop is layered ON single-player.
   Route per-player inside SP systems (P1 path stays, P2 path added
   alongside) rather than bypassing SP wholesale.
7. **Engine-wrapper layer vs gameplay/network layer.** Two separate
   subtrees:
   - `src/votv-coop/.../ue_wrap/` — one class per UE/VOTV engine class.
     Wraps reflection access, struct layouts, UFunction thunks. **No
     network logic, no gameplay logic, no coop state.**
   - `src/votv-coop/.../coop/` — network packets, interpolation, input
     buffers, `RemotePlayer`. Talks to the engine through `ue_wrap`.
   Communicate via clean APIs, not shared globals.

---

## UE4-specific adaptation of the methodology

`docs/COOP_METHODOLOGY.md` was written around native-engine RE (GTA:SA /
MTA). VOTV is UE4.27 — a known, documented engine. The principles are
unchanged; the *techniques* shift:

- **Discovery is reflection, not blind RE.** UE exposes every class,
  property, and function via `GUObjectArray` / `GNames`. UE4SS surfaces
  this. Phase 1 "engine archaeology" is mostly reflection introspection,
  not IDA work. Drop to raw RE only for things reflection can't see.
- **Injection / hooking is UE4SS, not hand-rolled MinHook.** UE4SS owns
  the injection, the UFunction hook engine, and Blueprint mod loading.
  WP13 (use established libraries) → UE4SS is that library here.
- **"Spawn the orphan" = spawn a 2nd `APawn` + `APlayerController`.** UE
  natively supports multiple controllers/pawns (split-screen, listen
  servers do it). The factory is `UWorld::SpawnActor`. This is far less
  crash-prone than the GTA orphan, but VOTV's SP-only assumptions will
  still need per-site fixes (principle 4).
- **MTA stays the conceptual precedent** for the parallel class hierarchy,
  the keysync packet, sessions, host-authoritative AI, cutscene sync. The
  *shapes* port; the *mechanism* is UE4SS.
- **Do NOT rely on UE's built-in replication for gameplay.** VOTV's
  blueprints were authored single-player (no `Replicated` props, no RPCs).
  Adding them means editing assets (A6). We run a custom UDP layer and
  drive the engine via replicated *input* + state push.

## Versioning — mirror the game version

The mod's version tag **mirrors the VOTV game version it targets**, to avoid
confusion about compatibility. The current target is **Alpha 0.9.0-n**
(the latest game release), so the mod is tagged `0.9.0-n`. When the mod is
brought up against a future game release, it carries that release's tag.
The game install folder name (`Game_0.9.0n/`) reflects the current target.

## Other standing rules

- Document findings + rename functions/objects (in IDB if doing RE; in
  research findings always). Verify, don't guess (WP4/WP18 — memory
  decays, current code/reflection is authority).
- Don't reinvent the wheel (WP13). Hooks/reflection: UE4SS.
- Mod menu / debug overlay: Dear ImGui (UE4SS ships an ImGui integration).
- Plain-English UI labels; technical depth in `(?)` tooltips (WP10).
- No emojis in code / files unless explicitly requested.

## Reading order after a session reset / new conversation

1. `MEMORY.md` index (auto-memory).
2. Top entry of `memory/` (latest project state).
3. `CLAUDE.md` (this file).
4. `docs/FEASIBILITY.md` (Phase 0 deliverable — current viability state).
5. `docs/COOP_METHODOLOGY.md` for architecture decisions.
6. `reference/RE-UE4SS/` for the UE4 modding substrate.
7. `reference/mtasa-blue/` for conceptual precedent.
8. `docs/COOP_SCOPE.md` for scope decisions.
