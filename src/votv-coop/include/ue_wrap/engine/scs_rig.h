// ue_wrap/scs_rig.h -- instantiate a cooked Blueprint class's cosmetic
// component rig (SimpleConstructionScript subset) onto a foreign actor.
//
// WHY: VOTV's kerfur variant actors (kerfurOmega_C + children) carry their
// visual identity as SCS components -- joint-life particles, the belly
// point light, mynet's static-electricity emitters / digital-grid decals /
// spark audio loops. The coop skin system dresses a PLAYER body in those
// variants' MESHES; this module brings the matching component rig along by
// reading the variant class's own SCS data at runtime (templates, attach
// bones, relative transforms) -- no hand-copied effect tables, the game's
// class stays the single source of truth (RE ground truth:
// research/pak_re/dump_scs.py over kerfurOmega{,_mynet}.json, 2026-07-03).
//
// Cosmetic subset = the classes that are pure presentation:
//   ParticleSystemComponent (with a Template)  -> UGameplayStatics::SpawnEmitterAttached
//   DecalComponent                             -> UGameplayStatics::SpawnDecalAttached
//   PointLightComponent                        -> AActor::AddComponentByClass (deferred)
//   AudioComponent named "eff_*" (game's own   -> UGameplayStatics::SpawnSoundAttached
//     effect-audio naming; excludes behavioral
//     Audio/kerfurEXE meow machinery)
// Everything else in the SCS (arrows, springarms, billboards, hitboxes,
// nav invokers, camera child actors, the mat_invisRender render probe) is
// actor BEHAVIOR, not skin cosmetics -- skipped.
//
// TEMPLATE-faithful flags: nodes the game authors OFF stay off -- a particle
// with bAutoActivate=false or a light with bVisible=false (kerfurOmega's
// makeSentient-only joint-life sparks + lifeLight) is not instanced at all --
// and authored behavior flags are copied onto the spawned instance
// (bAbsoluteLocation/Rotation/Scale via SetAbsolute, PrimaryComponentTick.
// bStartWithTickEnabled via SetComponentTickEnabled, AudioComponent
// AttenuationSettings). Bitfield flags are read BIT-EXACTLY off the template
// via reflection::FindBoolProperty (the take-2 byte-XOR heuristic died on a
// template overriding two flags in one packed byte); see TemplateFlag /
// ApplyTemplateFidelity.
//
// Engine-wrapper layer (principle 7): no coop/network state, no per-skin
// policy -- the caller decides WHICH classes to instance and owns the
// returned component pointers' lifecycle. Game thread only.

#pragma once

#include <vector>

namespace ue_wrap::scs_rig {

// Instantiate `bpClass`'s own SCS cosmetic nodes (this class's SCS only --
// walk the Super chain yourself if you want inherited rigs; the kerfur skin
// caller applies base-class + variant-class passes explicitly) onto `actor`:
//   - bone-anchored nodes (AttachToName / parent CharacterMesh0) attach to
//     `meshComp` at that bone with the template's relative transform;
//   - root/scene-anchored nodes attach to `rootComp`.
// Appends every created component (PSC / decal / audio / light) to
// `outComponents`. Returns the number created. Safe no-op (returns 0) when
// the class has no SCS or nothing passes the cosmetic filter.
int InstantiateCosmetics(void* actor, void* meshComp, void* rootComp,
                         void* bpClass, std::vector<void*>& outComponents);

}  // namespace ue_wrap::scs_rig
