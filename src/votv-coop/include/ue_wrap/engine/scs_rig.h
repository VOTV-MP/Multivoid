// ue_wrap/engine/scs_rig.h -- instantiate a cooked Blueprint class's cosmetic component rig (the
// SimpleConstructionScript subset) onto a foreign actor.
//
// VOTV's kerfur variant actors (kerfurOmega_C and its children) carry their visual identity as SCS
// components: joint-life particles, the belly point light, mynet's static-electricity emitters,
// digital-grid decals and spark audio loops. The coop skin system dresses a PLAYER body in those
// variants' MESHES; this module brings the matching component rig along by reading the variant
// class's own SCS data at runtime -- templates, attach bones, relative transforms -- so there is no
// hand-copied effect table and the game's class stays the single source of truth.
//
// Engine-wrapper layer (principle 7): no coop or network state and no per-skin policy -- the caller
// decides WHICH classes to instance and owns the returned component pointers' lifecycle. Game
// thread only.

#pragma once

#include <vector>

namespace ue_wrap::scs_rig {

// Instantiate `bpClass`'s own SCS cosmetic nodes onto `actor` -- this class's SCS only; walk the
// Super chain yourself for inherited rigs, as the kerfur skin caller does with a base pass and a
// variant pass. A bone-anchored node attaches to `meshComp` at its AttachToName bone with the
// template's relative transform; a root- or scene-anchored one attaches to `rootComp`. Created
// components are appended to `outComponents` and counted.
//
// Cosmetic means pure presentation, and each kind has its own spawner in the .cpp:
//   ParticleSystemComponent with a Template  -> SpawnEmitterAttached
//   DecalComponent                           -> SpawnDecalAttached
//   AudioComponent named "eff_*"             -> SpawnSoundAttached
//   PointLightComponent                      -> AddComponentByClass, deferred
// "eff_*" is the game's own effect-audio naming, excluding the behavioural Audio and kerfurEXE
// machinery; everything else in the SCS is actor BEHAVIOUR and is skipped. A node authored OFF
// stays off, and the authored flags are copied on by ApplyTemplateFidelity, whose bitfields
// TemplateFlag resolves BY NAME -- a byte heuristic cannot survive two flags in one packed byte.
int InstantiateCosmetics(void* actor, void* meshComp, void* rootComp,
                         void* bpClass, std::vector<void*>& outComponents);

}  // namespace ue_wrap::scs_rig
