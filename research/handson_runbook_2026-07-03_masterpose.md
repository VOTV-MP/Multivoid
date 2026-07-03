# Hands-on runbook 2026-07-03 (take 5) — ragdoll MASTER-POSE probe (all 6 bones, not just pelvis)

**Deployed:** DLL `4CFA16CEA15F1A94` on all 4 installs (hash-verified). Protocol UNCHANGED (v95 —
the probe is display-only, no wire change; peers on the previous `5A60579C` DLL stay compatible,
but all 4 installs already carry the new one). Paks unchanged.

**Status: BUILT + audited, NOT smoke-launched** — you were on the PC, so no autonomous launches;
the verdict is visual anyway (that is the point of the probe). Mechanism risk while OFF: zero
(one atomic load + map lookup per ragdoll frame; nothing anywhere else).

## What this answers (your ask)

"pelvis attach достаточно, но может еще какие кости КРОМЕ pelvis удастся прикрепить?"

Per-bone attaches are impossible (a component has ONE attach parent; the game has no runtime
per-bone write — bone writes are AnimGraph-only). But the engine has a one-call full-skeleton
coupling: `SetMasterPoseComponent` — the puppet's two kel body meshes become pose SLAVES of the
mirror ragdoll body's simulating mesh (full bone copy by NAME, engine-side, every frame).

**Static prognosis (new fact, 2026-07-03):** the kel player-body skeleton has exactly SIX bones —
`lowlegs, thighs, pelvis, chest, head, head_end` (tools/client_model/SPEC.md ReferenceSkeleton,
parsed from the cooked mesh) — and the ragdoll body rig is the same 6-bone family (bone overlay
counted 6; `pelvis` name-matches already). So expected coverage is **6/6 = the kel copies the
ENTIRE ragdoll pose**: legs, torso and head each follow their physics body instead of the whole
kel tumbling as one rigid piece around the pelvis. Custom client models keep working (they are
rigged onto the same 6-bone kel skeleton by our converter).

The pelvis attach + streamed pelvis rotation stay EXACTLY as before (position/orientation anchor);
master-pose lays the remaining 5 bones on top. While coupled, the probe also pins the kel mesh
components onto the body mesh component each frame (master-pose copies bones in COMPONENT space —
without the pin the pose would render rigidly offset).

## Test (host + client, both on `4CFA16CE`)

1. Host: **F1 → Player → HUD** → tick **"Ragdoll master-pose (probe)"** (next to the bone
   visualizer — tick that too if you want the skeleton lines over the flop).
2. Client: ragdoll (press **C**; a faint/trip works too) near the host, wait ~5 s, get up. Repeat
   a few times, including one flop thrown off a height/slope.
3. **Judge by eye on the host**: with the probe ON the flopping kel should bend — legs/torso/head
   following the physics bodies — vs the old rigid one-piece tumble. Toggle the checkbox mid-flop
   to A/B live (it applies/restores on the fly).
4. **Recover check**: after get-up the kel must stand + animate NORMALLY (walk anim, head look).
   A kel stuck in a flop pose or floating at a weird offset after recover = restore bug — grab the
   log.
5. **Host log** (`Game_0.9.0n\...\Win64\votv-coop.log`) must show per flop:
   - `[MASTER-POSE] applied puppet=... master=... (both kel slots slaved; world-pin active)`
   - `[MASTER-POSE] coverage 6/6 master-rig bones exist on the kel slave: lowlegs=Y thighs=Y
     pelvis=Y chest=Y head=Y head_end=Y` — **this line is the ground truth**. Anything under 6/6
     names the bone that did NOT couple (it rides its ref pose instead) — report the exact line.
   - `[MASTER-POSE] restored puppet=... (recover)` on get-up.
6. Also worth one look: your OWN ragdoll on the client's screen (the client can't enable the probe
   — dev-gated host-only — so the client still sees the old rigid tumble; that asymmetry is
   expected for a probe).

## If the verdict is "лучше" (better)

Say the word and I promote it: the master-pose coupling becomes the default ragdoll display (no
checkbox, RULE 2 — the plain rigid-attach look goes), both peers get it, and the checkbox/ini flag
retire.

## Known boundaries

- Client-side observers keep the rigid look (dev gate) until promotion.
- The mirror body is invisible BY DESIGN; the kel is the visible thing. If with the probe ON the
  kel visually LAGS the skeleton-overlay lines by a frame, that is the master-pose evaluation
  order — report it, there is a follow-up lever (tick prerequisites), don't live with it silently.
- The `[dev] ragdoll_master_pose=1` ini key force-enables at boot (for a future autonomous
  verify); the checkbox is the intended path for this test.
