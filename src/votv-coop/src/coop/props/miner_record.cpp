// coop/props/miner_record.cpp -- the class gate on a client's save record of a mining rig. See coop/props/miner_sync.h.
//
// A rig travels through a hand (the store sells it), and a drop's record carries its counts: the record lane takes a client's
// record for the actor its drop made. This gate lets one of a rig land on the host only when (a) the record is the one the
// sender's own drop awaits (prop_save_data::AwaitsRecordFrom, set by the drop lane's ExpectRecordFor), and (b) every number
// is one the game can reach (coop/props/miner_bounds.h). Any other record -- a client's edit of a world rig it did not drop,
// a number past the bounds -- is refused, final for that record, and the host's state of the rig goes back to the sender. A
// rig in a hand is its holder's inventory, which every hand lane takes as the holder's word (coop/player/hand_spend); the gate
// holds what the host can see, a record for a rig in the world.
//
// Residual: the counts of a rig a client dropped from its own hand are the holder's word too, bounded by the game's own caps
// (16 GPUs, 16 slots, 6 coolers, 512 points), so a modified client can drop a rig fuller than it picked it up.

#include "coop/props/miner_sync.h"

#include "miner_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/props/miner_bounds.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"

#include <string>

namespace coop::miner_sync {
namespace {

namespace M  = ue_wrap::miner;
namespace SR = ue_wrap::save_record;

// The records the gate let through, until their rig shows the counts: the record lands after the gate says yes (it may park
// first), so the host's own rig tells when it did. A place whose rig is gone is free again.
constexpr size_t kMaxAccepted = 8;
struct Accepted {
    ue_wrap::CachedObjRef actor;
    std::wstring key;
    M::Fields fields;
    bool used = false;
};
Accepted g_accepted[kMaxAccepted];
size_t g_acceptedCount = 0;
size_t g_nextAccepted = 0;
uint32_t g_refused = 0, g_applied = 0;
bool g_registered = false;

bool RegisterOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::miner_record_gate_off);
    return off;
}

bool Refuse(void* actor, uint8_t slot, const std::wstring& key, const char* why) {
    const uint32_t n = ++g_refused;
    if ((n & (n - 1)) == 0)
        UE_LOGW("miner_sync: HOST record refused %ls from slot %u: %s (#%u)", key.c_str(), static_cast<unsigned>(slot), why, n);
    detail::SendState(actor, slot);  // the sender's copy is set right by the host's own numbers (MTA CGame.cpp:2775-2790)
    return false;
}

void Forget(Accepted& a) {
    if (!a.used) return;
    a = Accepted{};
    --g_acceptedCount;
}

void Note(void* actor, const std::wstring& key, const M::Fields& f) {
    for (Accepted& a : g_accepted)
        if (a.used && a.key == key) {
            a.actor.Set(actor);
            a.fields = f;
            return;
        }
    Accepted* place = nullptr;
    for (Accepted& a : g_accepted) {
        if (a.used && !a.actor.Alive()) Forget(a);
        if (!a.used && !place) place = &a;
    }
    if (!place) {  // every place holds a live rig's record: the oldest gives way
        place = &g_accepted[g_nextAccepted];
        g_nextAccepted = (g_nextAccepted + 1) % kMaxAccepted;
        Forget(*place);
    }
    Accepted& slot = *place;
    ++g_acceptedCount;
    slot.used = true;
    slot.actor.Set(actor);
    slot.key = key;
    slot.fields = f;
}

bool Judge(void* actor, uint8_t slot, const SR::SaveRecord& rec) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    if (key.empty() || !coop::prop_save_data::AwaitsRecordFrom(key, slot))
        return Refuse(actor, slot, key, "no drop of this sender awaits it");
    M::Fields f;
    if (!M::ReadRecord(rec, f)) return Refuse(actor, slot, key, "it is not a rig's record layout");
    if (!InBounds(f.gpus, f.slots, f.fans, f.points, f.overheat)) return Refuse(actor, slot, key, "a number is past the bounds");
    Note(actor, key, f);
    return true;
}

}  // namespace

namespace detail {

void RegisterRecordGate() {
    if (g_registered) return;
    g_registered = true;
    if (RegisterOff()) {
        UE_LOGI("miner_sync: the rig's record gate is not registered (miner_record_gate_off)");
        return;
    }
    coop::prop_record_gates::RegisterIntentRecordGate(L"prop_miner_C", &Judge);
}

bool RecordsWait() { return g_acceptedCount > 0; }

void NoteRecordSeen(void* rig, const M::Fields& now) {
    for (Accepted& a : g_accepted) {
        if (!a.used) continue;
        if (!a.actor.Alive()) {  // its rig ended before it showed the counts
            Forget(a);
            continue;
        }
        if (!a.actor.Is(rig)) continue;
        if (now.gpus != a.fields.gpus || now.slots != a.fields.slots || now.fans != a.fields.fans) continue;
        ++g_applied;
        UE_LOGI("miner_sync: HOST record applied %ls: %d GPUs in %d slots, %d coolers", a.key.c_str(), now.gpus, now.slots, now.fans);
        Forget(a);
    }
}

void ClearRecord() {
    if (g_refused || g_applied) UE_LOGI("miner_sync: session end -- rig records refused %u, applied %u", g_refused, g_applied);
    for (Accepted& a : g_accepted) a = Accepted{};
    g_acceptedCount = 0;
    g_nextAccepted = 0;
    g_refused = g_applied = 0;
}

}  // namespace detail

uint32_t RecordsRefused() { return g_refused; }
uint32_t RecordsApplied() { return g_applied; }

}  // namespace coop::miner_sync
