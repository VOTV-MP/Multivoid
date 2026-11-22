// coop/props/bucket_hold_ledger.cpp -- see coop/props/bucket_hold_ledger.h.

#include "coop/props/bucket_hold_ledger.h"

#include "coop/net/protocol.h"
#include "coop/player/hand_item.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <string>

namespace coop::bucket_hold {
namespace {

namespace B = ue_wrap::bucket;

// One slot's hold: the numbers of the item it holds, from the take, and the hold generation the ledger was first read under.
struct Ledger {
    bool bound = false;
    uint32_t gen = 0;
    bool bagSeeded = false;
    int32_t bag = 0;
    bool concreteSeeded = false;
    int32_t concrete = 0;
};
coop::roster_ledger::PerSlotState<Ledger> g_ledgers;   // empty with the slot's occupant

// A take the host heard and no hold has claimed yet: the destroyed copy's class and units, and the hold generation it came under.
struct Pending {
    bool has = false;
    bool concrete = false;
    uint32_t gen = 0;
    int32_t units = 0;
    std::wstring cls;
};
coop::roster_ledger::PerSlotState<Pending> g_pending;
bool g_listening = false, g_defaultSaid = false;

bool InRoster(uint8_t slot) { return slot >= 1 && slot < coop::net::kMaxPeers; }

// The slot's ledger for its hold now. The ledger takes the hold generation of its first read; a later generation is a new hold, and
// the old numbers are gone. A pending take is claimed once the generation has moved past the one it came under: by the hold of its
// class (the class the hand held last, so a drop's record still finds it) when that is a hold of the taken item, and dropped
// otherwise.
Ledger& Current(uint8_t slot) {
    Ledger& l = g_ledgers[slot];
    const uint32_t gen = coop::hand_item::HoldGeneration(slot);
    if (!l.bound) {
        l.bound = true;
        l.gen = gen;
    } else if (l.gen != gen) {
        l = Ledger{};
        l.bound = true;
        l.gen = gen;
    }
    Pending& p = g_pending[slot];
    if (p.has && p.gen != gen) {
        if (coop::hand_item::LastHeldClassIs(slot, p.cls.c_str())) {
            if (p.concrete) {
                l.concreteSeeded = true;
                l.concrete = p.units;
            } else {
                l.bagSeeded = true;
                l.bag = p.units;
            }
        }
        p = Pending{};
    }
    return l;
}

// A client's granted take destroyed the host's copy of a bag or a concrete bucket: its units are kept for the hold that follows.
void OnTakeHeard(int senderSlot, void* actor) {
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    const B::Kind kind = B::KindOf(actor);
    Pending p;
    if (kind == B::Kind::Bag) {
        int32_t maxUnits = 0;
        if (!B::ReadBag(actor, p.units, maxUnits)) return;
    } else if (kind == B::Kind::Concrete) {
        B::Concrete c;
        if (!B::ReadConcrete(actor, c)) return;
        p.units = c.units;
        p.concrete = true;
    } else {
        return;
    }
    const uint8_t slot = static_cast<uint8_t>(senderSlot);
    p.has = true;
    p.gen = coop::hand_item::HoldGeneration(slot);
    p.cls = ue_wrap::reflection::ClassNameOf(actor);
    // The hold announce can come first: a hold that already names the taken class and has no count yet takes the units at once.
    if (coop::hand_item::HeldClassIs(slot, p.cls.c_str())) {
        Ledger& l = Current(slot);
        bool& seeded = p.concrete ? l.concreteSeeded : l.bagSeeded;
        if (!seeded) {
            seeded = true;
            (p.concrete ? l.concrete : l.bag) = p.units;
            return;
        }
    }
    g_pending[slot] = p;
}

}  // namespace

void Install() {
    if (g_listening) return;
    g_listening = true;
    coop::remote_prop::AddDestroyListener(&OnTakeHeard);
}

int32_t BagUnits(uint8_t slot) {
    if (!InRoster(slot)) return 0;
    Ledger& l = Current(slot);
    if (!l.bagSeeded) {
        l.bagSeeded = true;
        l.bag = kBagUnitsDefault;
        if (!g_defaultSaid) {
            g_defaultSaid = true;
            UE_LOGW("bucket_hold: slot %u holds a cement bag the host never saw taken from the world -- its units start at "
                    "the class default %d (said once)", static_cast<unsigned>(slot), static_cast<int>(kBagUnitsDefault));
        }
    }
    return l.bag;
}

void SetBagUnits(uint8_t slot, int32_t units) {
    if (!InRoster(slot)) return;
    Ledger& l = Current(slot);
    l.bagSeeded = true;
    l.bag = units;
}

int32_t ConcreteUnits(uint8_t slot) {
    if (!InRoster(slot)) return -1;
    const Ledger& l = Current(slot);
    return l.concreteSeeded ? l.concrete : -1;
}

void OnDisconnect() {
    for (uint8_t s = 1; s < coop::net::kMaxPeers; ++s) {
        g_ledgers[s] = Ledger{};
        g_pending[s] = Pending{};
    }
    g_defaultSaid = false;
}

}  // namespace coop::bucket_hold
