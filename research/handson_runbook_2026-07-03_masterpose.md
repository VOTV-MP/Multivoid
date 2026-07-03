# Hands-on runbook 2026-07-03 (take 5) — ragdoll display: VISIBLE PLUSHIE (take 2) + late fixes (take 3)

**Deployed (take 3):** DLL `3FDEF8097B7D937E` on all 4 installs (hash-verified;
supersedes `53332070` — same plushie display PLUS the nameplate head-bone anchor and the two
pile-dup fixes below). Protocol UNCHANGED (v95). Paks unchanged.

> **Take 4 PENDING DEPLOY:** the nameplate retest ("отстает") produced the Z-only smoothing
> refinement (`458f88ca`): X/Y raw (super snappy), only the height low-passed. Built as DLL
> `3DB6D4D554539866` but the running game instances locked the files — CLOSE BOTH GAMES and say
> the word; deploy takes seconds. Until then the installs run take 3 (full-vector smoothing).

**PLUSHIE VERDICT: VERIFIED hands-on 2026-07-03** — user: "your approach is amazing, so it
shows the kel_lmao skin when the ragdoll mode is active which is pretty cool. Thats one of the
best solutions." The take-2 section below is the as-built record.

## Take 3 — what to test on `3FDEF809`

1. **Nameplate** (user asks: "mount to the head bone of the current model" + "smooth out the
   movement"): the plate now rides the 'head' bone of whatever mesh the peer is rendered by —
   the plushie's head during a flop, the skin mesh's head otherwise — through a ~70 ms
   smoothing filter. Test: watch a peer walk/sprint (plate should track the body, no trailing),
   crouch (plate follows the head down), ragdoll (plate follows the flopping plushie head
   instead of going nuts), get up (snaps back clean).
2. **Pile dupe (the 11:49 chain)**: client joins while the host shuffles piles, then immediately
   spams E on the trash cluster during the first ~10 s. Expected: presses on not-yet-bound piles
   are CANCELLED (log: `[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile ... CANCELLED`), NO
   `net: NEW held actor ... prop_garbageClump_C key='None'` line ever appears on the client, and
   after a couple of seconds every pile grabs normally through the intent path. Host log while
   throwing piles: NO more `re-seed ... added N NEW` adopting its own land actors every 20 s.
3. Everything from take 2 below still applies (the plushie flop itself).

## Take 1 (master-pose probe) — REFUTED, deleted

The morning's master-pose probe ran exactly once in your hands-on (11:28): it applied and
restored cleanly, but the coverage line measured **4/6** — `lowlegs=MISS thighs=MISS pelvis=Y
chest=Y head=Y head_end=Y`. The visible skin's skeleton (native kel and the converted client
models alike) has NO bones named `thighs`/`lowlegs`, and master-pose maps strictly by name —
so the legs stayed rigid and the visual gain was just a slight torso bend. Your verdict:
"выглядит плохо". Probe + its ue_wrap helpers DELETED per RULE 2 (commit history keeps them).

## Take 2 (SHIPPED, default behavior — no checkbox, no ini): the plushie IS the display

Your read of the bone visualizer was exactly right: the ragdoll rig is one 6-link chain
(lowlegs-thighs-pelvis-chest-head-head_end) and the physics flops THAT. So now, when a remote
peer ragdolls:

- the spawned `playerRagdoll_C` body stays **VISIBLE** — the game's own plushie ragdoll, its
  mesh natively rigged to the full chain (this is literally what SP shows in mirrors when YOU
  ragdoll); its pelvis velocity is still slaved to the sender (v22), so the flop tracks them;
- the puppet's two kel body meshes are **HIDDEN for the flop** (visibility only — the custom
  skin asset keeps its reference, no GC risk) and restored on get-up;
- the puppet actor still rides the pelvis attach as the position anchor (nameplate, recover
  hand-off). The old per-frame kel rotation drive is gone with the kel hidden.

## Test (both peers on `533320703118F756`)

1. Client ragdolls near the host (C / faint / trip), gets up after a few seconds. Repeat,
   including one throw off a slope.
2. **Host judges by eye**: the flopping body should now be the full floppy plushie — legs,
   torso, head all bending — instead of a rigid standing kel glued at the pelvis. On get-up
   the normal kel (custom skin included) must come back standing + animating.
3. Host log per episode:
   - `ragdoll_body: spawned VISIBLE playerRagdoll_C flop body ...`
   - `RagdollDisplay::Start: VISIBLE plushie body=... puppet pelvis-attached + kel meshes hidden`
   - `RagdollDisplay::Stop: kel meshes restored, puppet detached, plushie body destroyed`
4. Bone visualizer (F1 > Player > HUD) still works — the cyan chain should now sit INSIDE the
   visible plushie.

## Known boundaries (report if they bother you)

- A peer using a CUSTOM client model shows the DEFAULT kel plushie while flopped (the body
  self-configures `kel_lmao`/`inst_kel_body`); the custom skin returns on get-up. Making the
  plushie wear the custom skin is a follow-up if wanted.
- The nameplate rides the puppet anchor (pelvis attach), not the plushie's head — may float
  slightly off the body mid-flop. Cosmetic.
- A skin change arriving MID-flop may pop a standing kel over the plushie until get-up (rare
  edge; the log will show it).
