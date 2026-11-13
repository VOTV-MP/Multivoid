// coop/player/hand_spend.cpp -- see coop/player/hand_spend.h.

#include "coop/player/hand_spend.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "coop/player/hand_item.h"
#include "coop/player/roster_ledger.h"  // PerSlotState

namespace coop::hand_spend {
namespace {

// The drill's red arms (config row hand_spend_drill), read here only, by the rule each turns off.
bool Arm(const char* value) { return coop::config::ResolveEnum(::coop::config_registry::rows::hand_spend_drill) == value; }
bool Unchecked() { static const bool on = Arm("hand_spend_unchecked"); return on; }  // the held class is not checked
bool NoLatch() { static const bool on = Arm("hand_spend_no_latch"); return on; }     // a hold's uses are not kept

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
    if (!Unchecked() && !coop::hand_item::LastHeldClassIs(slot, cls)) return false;
    Spent& sp = Current(slot);
    if (!NoLatch() && sp.uses >= uses) return false;
    ++sp.uses;
    return true;
}

}  // namespace coop::hand_spend
