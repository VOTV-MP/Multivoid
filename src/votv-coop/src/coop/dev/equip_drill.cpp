// coop/dev/equip_drill.cpp -- see coop/dev/equip_drill.h.

#include "coop/dev/equip_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/begin_equipment.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>

namespace coop::dev::equip_drill {
namespace {

// More wearables than a player has slots, so every free slot is offered one. The second fanny pack fits beside the first;
// a second flashlight of another kind is the game's "type already equipped".
constexpr const wchar_t* kWearables[] = {
    L"prop_equipment_watches_C",     L"prop_equipment_fannypack_C",    L"prop_equipment_fannypack_C",
    L"prop_equipment_flashlight_b_C", L"prop_equipment_headphones_C",  L"prop_equipment_emf_C",
    L"prop_equipment_geiger_C",      L"prop_equipment_goldMedallion_C", L"prop_equipment_pearlBracelet_C",
    L"prop_equipment_mittens_C",     L"prop_equipment_woolboots_C",    L"prop_equipment_bunnyears_C",
};

bool Enabled() {
    static const bool on = coop::config::ResolveString(::coop::config_registry::rows::equip_drill) == "run";
    return on;
}

bool g_done = false;
int g_session = 1;

// The peer is a player in a settled world: the host's own world has finished starting, a client's streams run. The hand-back
// is written into the save object before the world loads, so nothing after this changes the slots the drill reads.
bool WorldReady(coop::net::Session* s) {
    if (s->role() == coop::net::Role::Host) return coop::host_world_settle::Settled();
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
           coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced();
}

// The slots, the empty ones, and the list of what each holds.
struct Slots {
    int count = -1, empty = 0;
    std::wstring line;
};
Slots Read() {
    Slots s;
    ue_wrap::inventory::PlayerInventory inv;
    if (!ue_wrap::inventory::ReadAll(inv)) return s;
    s.count = static_cast<int>(inv.equipment.size());
    for (size_t i = 0; i < inv.equipment.size(); ++i) {
        const std::wstring& cls = inv.equipment[i].data.className;
        if (cls.empty()) ++s.empty;
        s.line += L" [" + std::to_wstring(i) + L"]" + (cls.empty() ? L"<empty>" : cls);
    }
    return s;
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || g_done || !s || !s->connected() || !coop::players::Registry::Get().Local()) return;
    if (!WorldReady(s)) return;
    // Nothing announces that the player's equipment can be read (a field the game writes in place); the world-ready terms
    // above stand for it, and a read that still fails waits for the next tick.
    const Slots before = Read();
    if (before.count < 0) return;
    g_done = true;
    const bool host = s->role() == coop::net::Role::Host;
    const char* role = host ? "host" : "client";
    const char* tag = host ? "equip-host" : "equip-client";
    if (before.empty == 0) {
        coop::rig_verdict::Say(tag, coop::rig_verdict::Outcome::Invalid,
                               "session %d: none of the %d equipment slots is empty, nothing to measure", g_session,
                               before.count);
        return;
    }
    UE_LOGI("[EQUIP-DRILL] %s: %d slots, %d empty:%ls", role, before.count, before.empty, before.line.c_str());
    int fit = 0;
    for (const wchar_t* c : kWearables) {
        const bool ok = ue_wrap::begin_equipment::GiveFromClass(c);
        if (ok) ++fit;
        UE_LOGI("[EQUIP-DRILL] %s: equip %ls -> %s", role, c, ok ? "fit" : "refused");
    }
    const Slots after = Read();
    UE_LOGI("[EQUIP-DRILL] %s: after the list, %d empty:%ls", role, after.empty, after.line.c_str());
    if (after.empty == 0 && fit == before.empty)
        coop::rig_verdict::Say(tag, coop::rig_verdict::Outcome::Pass,
                               "session %d: each of the %d empty slots took an item", g_session, fit);
    else
        coop::rig_verdict::Say(tag, coop::rig_verdict::Outcome::Fail,
                               "session %d: %d of %d empty slots took an item, %d still empty", g_session, fit,
                               before.empty, after.empty);
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_done = false;
}

}  // namespace coop::dev::equip_drill
