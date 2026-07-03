# Hands-on runbook 2026-07-03 — kerfur skin EFFECTS (RT face / mynet rig / step FX) — take 2

**Deployed (take 2): DLL `41D23E3320F6E173` on all 4 installs** (hash-verified; protocol v95).

**Take-2 fix after your "переборщил по скейлу" + "mynet is also broken" (the pink blast):**
take-1 force-enabled the SENTIENT-only add-ons — the belly point light (bVisible=false in the
game's own template!), the 14 joint-life spark emitters (bAutoActivate=false) and the 'ag'
glow MID. That pink flood was OUR light+sparks, stacked on every kerfur skin (mynet included —
its rig rides on the base pass, hence "also broken"). The rig is now TEMPLATE-FAITHFUL:
dormant-by-authoring nodes are not instanced at all (bitfield flags read via
template-vs-CDO byte XOR), the 'ag' glow block is deleted. What remains per skin is exactly
what the game's own crafted kerfur shows: mesh emissive + RT face (4 omegas) + mynet's own
always-on electricity + step FX + keljoy squeak.

NOTE if your reference robots (the two-natural-kerfurs photo) DO show a chest glow orb: those
are SENTIENT-crafted kerfurs. Say the word and I'll add the glow back as a TONED-DOWN option.

Earlier audit state still applies: perf 0-CRIT (3 WARNs fixed: load-miss latch, ONE stride
gate on the puppet, face-class lookup), correctness 0 findings. NO autonomous smoke (you are
at the PC).

## What shipped (root fix per rule 1, user reports "kerfur omega skin has no RT screens etc."
## + "kerfur_mynet has no footstep particles")

A builtin skin was only the MESH; the matching kerfur VARIANT ACTOR carries the rest of the
look. Now, whenever a builtin kerfur skin lands on a body (your own local body AND every
puppet), the coop layer rebuilds the variant's cosmetic identity, data-driven from the game's
own classes (no hand-copied effect tables — the SCS templates/CDO are read at runtime):

1. **Every kerfur skin** gets the base rig exactly as the game authors it for a crafted
   kerfur (take-2: the sentient-only sparks/light/'ag' glow stay OFF — template-faithful).
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
   atlas — and NO pink light blast. Client checks self in a mirror.
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
- `skin_effects: rig 'kerfur_omega' on <ptr> -- 0 SCS comp(s), face=YES (type=0 fmi=1), stepSound=no, stepBurst=no`
  (take-2: base pass instances NOTHING for a plain omega — its only SCS cosmetics are the
  dormant sentient set; mynet: ~31 comps, face=no, stepSound=YES, stepBurst=YES; keljoy:
  stepSound=YES)
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
