// coop/player/hand_spend.cpp -- see coop/player/hand_spend.h.

#include "coop/player/hand_spend.h"

#include "coop/player/hand_item.h"
#include "coop/player/roster_ledger.h"  // PerSlotState

namespace coop::hand_spend {
namespace {

// The hold generation of the last item a request spent, and the uses counted of it.
struct Spent { uint32_t gen = 0, uses = 0; };
coop::roster_ledger::PerSlotState<Spent> g_spent;

// The slot's record for its current hold: a hold announced after the last spend starts over.
Spent& Current(uint8_t slot) {
    Spent& sp = g_spent[slot];
    const uint32_t gen = coop::hand_item::HoldGeneration(slot);
    if (gen != sp.gen) {
        sp.gen = gen;
        sp.uses = 0;
    }
    return sp;
}

}  // namespace

bool Spend(uint8_t slot, const wchar_t* cls, uint32_t uses) {
    if (!coop::hand_item::LastHeldClassIs(slot, cls)) return false;
    Spent& sp = Current(slot);
    if (sp.uses >= uses) return false;
    ++sp.uses;
    return true;
}

void Burn(uint8_t slot, const wchar_t* cls, uint32_t uses) {
    if (!coop::hand_item::LastHeldClassIs(slot, cls)) return;
    Spent& sp = Current(slot);
    if (sp.uses < uses) sp.uses = uses;
}

}  // namespace coop::hand_spend
