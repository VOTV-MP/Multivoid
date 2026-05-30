// coop/element/prop.h -- the Prop Element subclass.
//
// Third subclass of `coop::element::Element` (after Npc and Player). Each live
// keyed-interactable Aprop_C derivative (chipPile / clump / trashBitsPile +
// every Aprop_*_C food/container/etc) has a Prop Element shadow whose
// ElementId is the unified runtime address used for cross-subsystem dispatch.
//
// Dual-track identity (per the MTA audit section 3.5.2):
//   - `Element::m_name`    = the Aprop_C.Key string (save-stable, persists
//                            across game-save cycles -- the only stable id
//                            VOTV gives us for world-prop matching on load).
//   - `Element::m_typeName`= the BP class name (e.g. "Aprop_chipPile_C").
//   - `Element::m_id`      = the runtime ElementId (host-allocated; not save-
//                            stable; the wire-efficient handle used by
//                            PropPose / PropRelease / etc).
//   - `Element::m_actor`   = the live AActor* (engine-owned; we don't manage
//                            its lifetime, the engine does).
//
// Lifetime: owned by the shared `coop::element::MirrorManager<Prop>::Instance()`
// singleton (PR-FOUNDATION-3 Inc3 2026-05-30 -- the bespoke g_propElementsById
// owner map was retired). The LOCAL handle is AllocAndInstall'd by
// `coop::prop_element_tracker::MarkPropElement` (m_mirror=false) when
// `MarkKnownKeyedProp` fires from the Init POST observer or the seed scan, and
// Take'n out when `UnmarkKnownKeyedProp` fires from K2_DestroyActor PRE; the
// MIRROR handle is Install'd by `coop::remote_prop::RegisterPropMirror`
// (m_mirror=true) for a wire-received prop. Both kinds share the one manager.
//
// Per the audit (section 4.3): `CClientObject` is MTA's world-prop equivalent
// of `Aprop_C`. The Prop Element here is the same architectural shape (id +
// type tag + name + actor + lifecycle flag) minus MTA's CClientStreamElement
// position/streaming machinery (UE owns transforms; PropPose handles the
// wire side).

#pragma once

#include "coop/element/element.h"

namespace coop::element {

class Prop : public Element {
public:
    Prop() : Element(ElementType::Prop) {}
};

}  // namespace coop::element
