// coop/dev/equip_drill.cpp -- see coop/dev/equip_drill.h.

#include "coop/dev/equip_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/begin_equipment.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace coop::dev::equip_drill {
namespace {

constexpr uint64_t kSettleMs = 15000;

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

uint64_t g_readyMs = 0;
bool g_done = false;
int g_session = 1;

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
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && (!coop::net_pump::HasAnnouncedWorldReady() ||
                  coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle))
        return;
    const uint64_t now = ::GetTickCount64();
    if (!g_readyMs) { g_readyMs = now; return; }
    if (now - g_readyMs < kSettleMs) return;
    g_done = true;
    const char* role = host ? "host" : "client";
    const Slots before = Read();
    if (before.count < 0) {
        UE_LOGW("[EQUIP-DRILL] %s ABANDONED in session %d: the equipment does not read", role, g_session);
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
        UE_LOGI("[EQUIP-DRILL] %s DONE PASS in session %d: each of the %d empty slots took an item", role, g_session, fit);
    else
        UE_LOGW("[EQUIP-DRILL] %s DONE FAIL in session %d: %d of %d empty slots took an item, %d still empty", role, g_session,
                fit, before.empty, after.empty);
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_readyMs = 0;
    g_done = false;
}

}  // namespace coop::dev::equip_drill
