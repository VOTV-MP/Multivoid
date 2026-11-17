// coop/element/building.h -- the Building Element subclass, a peer of `Prop` under `coop::element::Element`:
// the identity row of a building piece (a nail, a wall, an unfinished wall, a poster). Like a prop it carries
// the actor_save key as its name and the blueprint class as its type name; unlike a prop it has no drive, no
// grab and no pile life, so it is its own type and the prop lane's eid reads never resolve it. Owned by
// `element::BuildingMirrors()`.

#pragma once

#include "coop/element/element.h"

namespace coop::element {

class Building : public Element {
public:
    Building() : Element(ElementType::Building) {}
};

}  // namespace coop::element
