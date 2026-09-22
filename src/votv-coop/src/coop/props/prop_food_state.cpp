// coop/props/prop_food_state.cpp -- see header.

#include "coop/props/prop_food_state.h"

#include "coop/props/prop_save_data.h"

namespace coop::props::prop_food_state {

void Install() {
    // One name covers the whole family: the record lane resolves the claim against the class and
    // its descendants, and every class that CARRIES this state is one of them -- including the ones
    // whose names say nothing about food, like prop_bananaHusk_C and prop_arirEgg_C. The test is
    // lineage, not the name: prop_food_mre_C is a prop_C and carries none of it.
    coop::prop_save_data::DeclareClassOwnedElsewhere(L"prop_food_C");
}

}  // namespace coop::props::prop_food_state
