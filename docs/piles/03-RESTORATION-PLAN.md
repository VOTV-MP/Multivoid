# Pile sync — restoration plan (restore the working reconcile)

> **`[corr 2026-09-04: docs census sweep]` SUPERSEDED. THIS PLAN WAS NEVER EXECUTED, AND ITS
> APPROACH WAS REPLACED.** It reads "Drafted 2026-06-20 (session 32). **PENDING USER GO** before
> execution" and carries "Open questions for the restore (resolve before/at execution)" -- live work
> that will never happen. `[V]` **`docs/piles/11-PROXY-TO-NATIVE-NATIVIZATION-2026-06-30.md`:
> *"Status: nativization AS-BUILT + VERIFIED (hands-on). The pile form is now a rooted real
> `actorChipPile_C` native"*** (`abfaaed8` / `dabf84de` / `3b72aba0` / `fa8bc344`, pushed). The pile
> mirror went PROXY -> NATIVE ten days after this plan was drafted, so "restore the adopt-by-position
> reconcile onto the proxy" describes an era the tree has since left behind.
>
> Its DELETE list is also already spent: `[V]` `prop_adoption.cpp`, `pile_handle.cpp`,
> `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `remote_prop_spawn_internal.h` are
> gone from the tree.
>
> **Kept, not archived,** because `README.md` deliberately retains `01-04` as the pile-specific
> history set ("These supersede the cross-cutting parts of the pile docs below; 01-04 remain the
> pile-specific..."). Read it as a record of what was proposed in June 2026, never as a plan.
> Current pile state: `docs/piles/11` and `12`, plus crutch **C2**.

> The plan to get back to working pile sync: restore the adopt-by-position reconcile (the
> June-12/13 "stale save → reconcile" scheme, intact at committed HEAD `1272b0a3`) and fix
> the one real edge case (drain-rot) cheaply, instead of the destroy+fresh-spawn detour that
> has duped for 10 sessions. Drafted 2026-06-20 (session 32). **PENDING USER GO** before execution.

## Principle

Stop fighting the keyless-pile identity with "destroy the client's pile and fresh-spawn a host
mirror + a doom to clean up." Instead **re-use the pile the client already loaded** — bind it
onto the host eid by position (ADOPT). One actor, no dupe, by construction. This is RULE 1
(root cause: the dupe is *making two*; adopt makes one) and RULE 2 (delete the failed
fresh-spawn/doom/resync apparatus, no parallel paths).

## What to restore (the target behaviour)

From HEAD `1272b0a3` (`EnsurePileBindIndex`, `remote_prop_spawn.cpp:498-563`):
- On a join snapshot bracket, for each host eid-only pile expression, match the client's OWN
  nearest save-loaded pile within **30 cm + equal chipType + exact class** → `RecordClaim` +
  `UnmarkKnownKeyedProp` + `ReconcileToHostPhysics` + `RegisterPropMirror(eid, ownPile)`. The
  client's pile BECOMES the mirror.
- Residue (piles the host collected/no longer has) removed by the quiescence-gated divergence
  sweep with the >50% world-wipe valve.
- Mid-session host pile spawns mirrored via the steady-world re-seed (`c527f31b`).
- Host-authoritative removal via the death-watch (`1272b0a3` PART 1) — already committed, KEEP.

## The one edge case to solve (cheaply, the RIGHT way)

The reason the team abandoned adopt: the join's same-world **shadow-drain** frees + re-creates
the client's save piles, so a bind made at the bracket rots. The cheap, correct answer (NOT a
new architecture):
- **Re-run the adopt after the drain.** When `net_pump` detects the mass-purge entry edge,
  re-arm the position-adopt over the (re-created) save piles vs the host eid set. The host eids
  are stable (host-side); the client's re-created piles are at the same positions → re-match.
- This replaces the entire PileResyncRequest/re-stream/doom apparatus: there is nothing to
  re-stream (the client still has its piles) and nothing to doom (the original IS the mirror).

## Restoration inventory (revert / keep / delete)

Source of truth: `git diff HEAD` (the uncommitted s23-s32 stack). The working scheme is at HEAD;
the churn is uncommitted, so this is a **working-tree restore**, NOT `git revert`. Several files
are ENTANGLED with non-pile uncommitted work — a blanket `git checkout HEAD -- <file>` is unsafe
on those; surgically remove only the pile hunks.

### SAFE to restore fully to HEAD (pile-mechanism only)
- `src/votv-coop/src/coop/remote_prop_spawn.cpp` — restores `EnsurePileBindIndex` adopt + P2
  claim/sweep (the −685-line revert). NOTE: HEAD's version is monolithic; restoring it means
  also handling the extracted `prop_adoption.cpp` (delete it).
- `src/votv-coop/include/coop/remote_prop_spawn.h` — re-declares the adopt API.

### DELETE (new untracked pile-only files — the thin-client experiment)
- `src/votv-coop/src/coop/prop_adoption.cpp` (the doom/catalog/seed engine)
- `src/votv-coop/src/coop/pile_handle.{h,cpp}` (the grab/throw relay — verify nothing committed
  depends on it; it is untracked, so safe to drop)
- `src/votv-coop/include/coop/remote_prop_spawn_internal.h`
- `src/votv-coop/src/coop/dev/freeze_probe.{h,cpp}` (thin-client drain instrumentation)
- the matching `CMakeLists.txt` entries (3 lines)

### ENTANGLED — surgically strip ONLY the pile hunks (keep the rest)
- `net_pump.cpp` — KEEP the committed PART-1 death-watch (at HEAD); remove the uncommitted
  `PileResyncRequest`/`ArmPileDoom`/`DrainPileReStreamChunk` hunks.
- `remote_prop.cpp` — KEEP kerfur `NotifyKerfurPropMirrorBound`; remove pile
  `NotifyPileMirrorBound`/`TryAdoptOrDeferConvert`/pending-remove.
- `prop_snapshot.cpp` — remove `ReStreamPilesToSlot`/`DrainPileReStreamChunk` (pile-only).
- `protocol.h` — remove v81-v88 pile kinds (`PileResyncRequest/Complete`, `PileGrabRequest`,
  payloads); verify no non-pile kind is interleaved (appears pile-only).
- `event_dispatch_state.cpp`, `event_feed.cpp`, `subsystems.cpp`, `session_lanes.h` — remove the
  pile ReliableKind routing (the 3-place wiring) only.
- `trash_collect_sync.cpp` — KEEP the committed PART-2 client host-authority gate; diff
  carefully vs HEAD (HEAD already touched this file).
- `prop_element_tracker.{cpp,h}` — restore the P1 mint-gate to `isKerfur` only (drop the
  `|| isChipPile` half, the Break-2 regression). KEEP the kerfur half.

### MUST PRESERVE (pure non-pile uncommitted work)
- `email_sync.cpp` (+106) — the 2026-06-19 email-flood RCA fix. Zero pile content.
- `kerfur_convert.cpp`, `kerfur_prop_adoption.cpp`, `npc_mirror.cpp` — kerfur work.
- `local_streams.cpp`, `remote_player.cpp`, `ui/hud.cpp`, `ui/imgui_overlay.cpp` — events/HUD.
- `harness/autotest*.{cpp,h}`, `tools/mp.py` — the new pile interaction smoke is additive test
  infra; KEEP (it is the long-missing interaction smoke — it will validate the restored scheme).

## Open questions for the restore (resolve before/at execution)

1. The HEAD `EnsurePileBindIndex` adopt was itself reported as "rotting" by s23-24. Confirm WHY
   it rotted (drain-frees-the-bound-pile) and that the **re-run-after-drain** fix addresses it —
   i.e. the adopt itself was sound, only its one-shot timing was wrong. (s23-24 memory + the
   `votv-snapshot-adoption-root-causes-2026-06-10.md` doc.)
2. Was the June-12/13 working scheme on the STALE `.sav` (pre-`de10514c`) specifically, and does
   the LIVE-capture switch (`de10514c`, for kerfurs) interact badly with the pile adopt? If the
   live capture is what destabilized piles, consider whether piles need the stale path while
   kerfurs need the live path (or whether the adopt works equally with both). The user explicitly
   said "stale save — reconcile" — verify whether the LIVE switch must be undone for piles.
3. The interaction (E-spam) dupe: under restored adopt, is the unbound-grab path gone (the pile
   is the client's own adopted actor, so it stays bound), or is the re-stream-trigger fix still
   needed as a backstop?

## Execution order (once user approves)

1. Restore `remote_prop_spawn.{cpp,h}` to HEAD; delete the new pile-only files; revert the P1
   gate to kerfur-only.
2. Surgically strip the pile hunks from the entangled files (keep kerfur/email/death-watch/P2).
3. Add the cheap **re-run-the-adopt-after-drain** edge (the only new code).
4. Build clean; adversarial audit (adopt correctness + the drain re-run + no non-pile breakage).
5. Deploy x4 + ground-prep + runbook; user hands-on. NEVER claim fixed from a smoke.
6. On clean hands-on: commit the unwound stack.

## DO NOT (the lessons этой saga encodes)
- Do NOT destroy the client's pile and fresh-spawn a mirror (that is the dupe machine).
- Do NOT gate a doom on a cross-population count that can't converge after the drain.
- Do NOT strip the save piles (the mirror sync ADOPTS them — strip → no piles).
- Do NOT add more reconcile band-aids on top; restore the working adopt and fix the ONE edge.
