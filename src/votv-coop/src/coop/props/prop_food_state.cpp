// coop/props/prop_food_state.cpp -- see header.

#include "coop/props/prop_food_state.h"

#include "coop/props/prop_save_data.h"

namespace coop::props::prop_food_state {

void Install() {
    // One name covers the whole family: the record lane resolves the claim with IsDescendantOfAny,
    // and every food -- including the ones whose names say nothing about food, like
    // prop_bananaHusk_C and prop_arirEgg_C -- is a descendant of this class.
    coop::prop_save_data::DeclareClassOwnedElsewhere(L"prop_food_C");
}

}  // namespace coop::props::prop_food_state
