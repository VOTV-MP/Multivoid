// ue_wrap/scs_rig.h -- instantiate a cooked Blueprint class's cosmetic component rig (the
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
// Super chain yourself for inherited rigs, as the kerfur skin caller does with an explicit base
// pass and variant pass. A bone-anchored node (AttachToName, or a CharacterMesh0 parent) attaches
// to `meshComp` at that bone with the template's relative transform; a root- or scene-anchored one
// attaches to `rootComp`. Every created component is appended to `outComponents` and the count
// returned; 0 when the class has no SCS or nothing passes the filter.
//
// Cosmetic means pure presentation: a ParticleSystemComponent carrying a Template, a
// DecalComponent, a PointLightComponent, and an AudioComponent named "eff_*" -- the game's own
// effect-audio naming, which excludes its behavioural Audio and kerfurEXE machinery. Everything
// else in the SCS is actor BEHAVIOUR and is skipped. A node the game authors OFF stays off, so a
// particle with bAutoActivate=false or a light with bVisible=false is never instanced, and the
// authored flags (absolute placement, tick-enabled, attenuation) are copied onto the instance.
// Bitfield flags are read BIT-EXACTLY through reflection::FindBoolProperty, since a byte-level
// heuristic cannot survive a template overriding two flags in one packed byte.
int InstantiateCosmetics(void* actor, void* meshComp, void* rootComp,
                         void* bpClass, std::vector<void*>& outComponents);

}  // namespace ue_wrap::scs_rig
