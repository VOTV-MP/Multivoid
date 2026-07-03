# Hands-on runbook 2026-07-03 — kerfur skin EFFECTS (RT face / alive glow / mynet rig / step FX)

**Deployed: DLL `64D470A02C36B7DE` on all 4 installs** (hash-verified; protocol v95 unchanged —
mixing with older peers is fine, effects are receiver-local). Includes both audit passes:
perf 0-CRIT with 3 WARNs fixed same session (load-miss latch, ONE stride gate on the puppet —
keljoy/mynet step FX land exactly on the native step, face-class lookup order) + correctness
0 findings (the FinishAddComponent transform nuance pre-empted by passing the template
transform through). NO autonomous smoke ran (you are at the PC — per the standing rule you
test, I prepared the ground); the log lines below are the build-proof on the FIRST launch.

## What shipped (root fix per rule 1, user reports "kerfur omega skin has no RT screens etc."
## + "kerfur_mynet has no footstep particles")

A builtin skin was only the MESH; the matching kerfur VARIANT ACTOR carries the rest of the
look. Now, whenever a builtin kerfur skin lands on a body (your own local body AND every
puppet), the coop layer rebuilds the variant's cosmetic identity, data-driven from the game's
own classes (no hand-copied effect tables — the SCS templates/CDO are read at runtime):

1. **Every kerfur skin** gets the base rig in its ALIVE (sentient) form — the user's reference
   screenshots: 14 joint-life spark emitters on the skeleton bones, the pink-purple belly
   point light, and the 'ag' glow texture (ear/markings emissive) on the body material.
2. **The 4 omega bodies** (kerfur_omega / _h / _m / _nc) get the REAL animated face: the
   game's own kerfusFace_C actor is spawned per body (256x256 scene-capture RT, its own
   blinking AnimBP), its dynmat goes into the mesh's screen slot. Face color = the variant's
   own Type: omega/nc = blue, m = pink, h = green.
3. **kerfur_mynet** additionally gets its full static-electricity rig (9 limb emitters, the
   pofinStatic bursts under the feet, 11 digital-grid decals under the body, 3 spark-loop
   audio comps) + per-step FX: an eff_mynetEmitterStep burst at the feet + the boltrix hit
   sound (with att_default attenuation, native parity).
4. **kerfur_keljoy** squeaks per step (its CDO footstepSound), like the real keljoy kerfur.
5. Step FX fire for puppets (stride of the interp position) AND your own body (stride of the
   wire pose sample) — you see your own mynet bursts looking down.
6. Ragdoll flop: the rig hides with the kel meshes (no floating sparks over the plushie),
   restores on get-up. Skin change / dr_kel / converter skins: full rig teardown (face actor
   destroyed, face slot cleared, light/particles/decals/audio removed).

## Tests

1. **Omega face (the screenshot report):** client picks `kerfur_omega`. Host looks at the
   client puppet: the face screen must show the LIVE blue cat face (blinking), not the teeth
   atlas; belly glows purple; joint sparks visible; ears glow. Client checks self in a mirror.
   Try `kerfur_omega_m` (pink face) and `_h` (green face) too.
2. **mynet (the particles report):** client picks `kerfur_mynet`. Expect standing: digital
   particles on limbs, grid glow under feet, spark crackle audio. Walking: burst + electric
   hit sound at each step — both on the puppet (host view) and your own feet (client view).
3. **keljoy:** squeak per step on both views.
4. **Ragdoll:** with any kerfur skin, C-ragdoll near the other peer — sparks/light/decals must
   vanish with the body (clean plushie), return on get-up.
5. **Skin switching:** omega -> mynet -> dr_kel -> a converter pak skin. Each switch: previous
   effects fully gone (no orphan light/particles, face slot back to normal), new ones arrive.
6. **Both-role check:** repeat 1-2 with HOST wearing the skin (client observes).

## Log proof (votv-coop.log)

- `skin_effects: skin 'kerfur_omega' <- variant 'kerfurOmega' (CDO skinMesh 'kerfurOmegaV1')`
- `skin_effects: rig 'kerfur_omega' on <ptr> -- 15 SCS comp(s), face=YES (type=0 fmi=1), stepSound=no, stepBurst=no`
  (mynet: ~35+15 comps expected, face=no, stepSound=YES, stepBurst=YES; keljoy: stepSound=YES)
- NO `scs_rig: property ... not found` warnings (would mean engine layout drift)
- NO `skin_effects: load MISS ...` (would mean a wrong asset path)

## Known boundaries

- Face is gated to the 4 omega bodies (other variant meshes have no screen slot; maid is a
  single-material mesh — measured).
- kerfur_maid / kerfur_krampus have no variant actor in the game: they wear the BASE alive
  rig, no face.
- The col (paintable) kerfur NPC's picked color is per-instance state and is NOT on the wire —
  separate note in the NPC-sync answer; unrelated to player skins.
- The kerfusFace actors live at world origin area (0,0,10) — same spot the game parks every
  real kerfur's face actor; nothing to see there (the capture shows only its own face mesh).
