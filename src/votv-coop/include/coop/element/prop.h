// coop/element/prop.h -- the Prop Element subclass. Third after Npc and Player: every live
// keyed-interactable Aprop_C derivative (chipPile, clump, trashBitsPile, every Aprop_*_C food or
// container) has a Prop Element shadow whose ElementId is the runtime address the rest of the mod
// dispatches on. The shape is MTA's `CClientObject` minus the streaming machinery: UE owns the
// transform, PropPose the wire side.
//
// Identity runs on two tracks. `m_name` is the Aprop_C.Key string, the only save-stable id the game
// offers, so a prop re-matches across save cycles; `m_id` is the runtime ElementId, host-allocated,
// not save-stable, the handle PropPose and PropRelease put on the wire. `m_typeName` is the
// blueprint class name, `m_actor` the live AActor*, whose lifetime is the engine's. Local and
// mirror handles share one owner, `element::PropMirrors()`: the local one AllocAndInstall'd by
// `prop_element_tracker::MarkPropElement` on `MarkKnownKeyedProp` (the Init POST observer or the
// seed scan) and Take'n back on `UnmarkKnownKeyedProp` (K2_DestroyActor PRE), the mirror one by
// `remote_prop::RegisterPropMirror`.

#pragma once

#include "coop/element/element.h"

namespace coop::element {

class Prop : public Element {
public:
    Prop() : Element(ElementType::Prop) {}
};

}  // namespace coop::element
