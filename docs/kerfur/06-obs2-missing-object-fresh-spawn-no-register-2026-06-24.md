# 06 — OBS-2: a save kerfur OBJECT missing on the client (fresh-spawn OnSpawn'd but never registered a mirror)

**Status: DIAGNOSIS IN-PROGRESS (started 2026-06-24, interrupted by /documentize). NOT built. Root NOT yet
pinned. Separate from the reverse follow-ghost (doc 05, CLOSED) -- surfaced in the same 13:15 verify session.**

## The symptom (user hands-on, 13:15 session)
Host has **5 kerfur OBJECTs** (turned-off props) + 1 active. Client shows **4 OBJECTs** + 1 active. The active
matched (reverse fix held, no ghost). **One off-prop kerfur OBJECT did not materialize on the client.**

**NOT the reverse / not in-window-turn_off:** the host log has ZERO `executing turn_off` / `BindFormActor ->prop`
this session -- so the missing object is a SAVE object, not a kerfur the host turned off in the window. The
reverse fix is COMPLETE; this is a distinct off-prop/save-object bug.

## What the log establishes (FACTS)
The missing object = key `s1l33pEyzXNNzdcPk_HGag` (eid 3150), at loc (1728.1, -534.0, 6135.6).
- Client received ALL 5 kerfur-object broadcasts (5 `remote_prop::OnSpawn cls='prop_kerfurOmega_C'`).
- **4 of 5 bound a mirror via a LOCAL save-twin** (`RegisterPropMirror` eid 4346-4349: keys gPXK / CZvy / p0KP /
  Nrby) -- these had a local save-loaded prop to adopt.
- **s1l33p (eid 3150) had NO local twin.** First OnSpawn (13:15:15) -> `kerfur-prop-adopt: armed deferred
  adoption eid=3150 (awaiting local twin)`. At quiescence (13:15:19) -> `kerfur-prop-adopt: eid=3150 -- no local
  twin (load tail quiesced); fresh-spawning a mirror` -> calls `remote_prop_spawn::OnSpawn(payload,
  senderSlot=0, localPlayer, deferKerfur=false)` ([kerfur_prop_adoption.cpp:150-151](../../src/votv-coop/src/coop/kerfur_prop_adoption.cpp#L150-L151)).
- **That fresh-spawn OnSpawn logged ONLY the OnSpawn line -- NO `Gap-I-1` match, NO `spawned ... of`, NO
  `RegisterPropMirror`, NO `collision restored`.** Total kerfur prop mirrors bound on the client = **4, not 5**.
  Session ran to 13:16:47 (88s), so it is NOT a truncation -- the 5th genuinely never registered.

## Two open sub-questions (to split by FACT before any fix)
1. **Why did s1l33p's save-twin NOT load on the client when the other 4 did?** Is s1l33p in the CLIENT's
   transferred save at all? If yes, why no local prop materialized (load-tail timing? a state difference? a
   key/position the local load placed elsewhere?). If no, why does the HOST broadcast it (host-save vs
   client-save divergence)? -- to grep: the client's local prop load for s1l33p (any local Init/MarkPropElement
   with that key) vs the 4 that twinned.
2. **Why did the fresh-spawn `OnSpawn(deferKerfur=false)` NOT register a mirror?** It correctly fell to
   fresh-spawn (no twin), OnSpawn ran, but no mirror bound. WHERE does OnSpawn fall through for this eid? Candidate
   paths to read in `remote_prop_spawn::OnSpawn`: an idempotency early-return (eid 3150 already known from the
   first deferred OnSpawn?), a Gap-I-1 fuzzy match to a non-registering actor, a spawn/Movable failure
   ([[lesson-runtime-staticmeshactor-must-be-movable]]), or a deferred re-arm that didn't fire.

## Connection to symptom 2 (camera) -- PRELIMINARY, to confirm
Symptom 2 (camera, doc 04 pending) is a fresh-spawn problem on the **NPC** path
(`npc_mirror::SpawnFreshNpcMirror`, the v74 floating-camera deferred spawn). OBS-2 is a fresh-spawn problem on the
**PROP** path (`remote_prop_spawn::OnSpawn`). **They are DIFFERENT fresh-spawn paths** (prop receiver vs NPC
mirror spawn), so PRELIMINARILY they do NOT share a root -- but confirm by reading both: if both fail at a shared
sub-step (e.g. a common Movable/registration helper), they could share a fix; if the prop OnSpawn fails for a
prop-specific reason (idempotency / fuzzy-match), OBS-2 is its own fix. **NOT yet determined.**

## Relation to off-prop scope A (doc 03) -- DISTINCT
Scope A is a **dedup** fix (twin-destroy the join-window DUPLICATE by save-time key). OBS-2 is a **MISSING**
object (no twin loaded + fresh-spawn didn't register). A dedup does NOT close a missing object -> **scope A will
not fix OBS-2.** Order (user 2026-06-24): diagnose+fix OBS-2 (visible) FIRST, then off-prop scope A.

## NEXT
Finish the two-sub-question grep + read `remote_prop_spawn::OnSpawn` to find the no-register fall-through, and
read `SpawnFreshNpcMirror` to confirm same-path-or-not vs the camera symptom. Do NOT build until the
no-register root is named (diagnosis-first, per the whole track).
