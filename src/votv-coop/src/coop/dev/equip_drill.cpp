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
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::equip_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// Every Blueprint addHint: AddEquipment says why it refused, and only "Not enough space" is the game finding no free slot.
// The other refusals ("This type is already equipped", "This item is already equipped") are the list's, not the game's.
UE_FACT_FN(kRowHint, "dev.equip_drill", nullptr, L"addHint", AnyClass);
constexpr int kTagHint = 0x45514450;   // 'EQDP'
constexpr const wchar_t* kNoSpace = L"Not enough space";

// More wearables than a player has slots, so every free slot is offered one. The second fanny pack fits beside the first;
// a second flashlight of another kind is the game's "type already equipped". A list that runs out of acceptable
// wearables before the slots are full says INVALID, never FAIL.
constexpr const wchar_t* kWearables[] = {
    L"prop_equipment_watches_C",     L"prop_equipment_fannypack_C",    L"prop_equipment_fannypack_C",
    L"prop_equipment_flashlight_b_C", L"prop_equipment_headphones_C",  L"prop_equipment_emf_C",
    L"prop_equipment_geiger_C",      L"prop_equipment_goldMedallion_C", L"prop_equipment_pearlBracelet_C",
    L"prop_equipment_mittens_C",     L"prop_equipment_woolboots_C",    L"prop_equipment_bunnyears_C",
};

// The bound on a world that never settles, a hint watch that never goes live, or an equipment that never reads: it says
// INVALID and nothing else.
constexpr uint64_t kReadyBoundMs = 300000;

bool Enabled() {
    static const bool on = coop::config::ResolveString(::coop::config_registry::rows::equip_drill) == "run";
    return on;
}

bool g_done = false;
int g_session = 1;
uint64_t g_startMs = 0;
bool g_hintWatch = false;
bool g_noSpaceSeen = false;

sg::Verdict OnHintPre(const sg::Call& c) {
    static R::ParamOffset sText{L"InText"};
    const int32_t o = sText.Of(c.function);
    if (o >= 0 && c.locals && ue_wrap::ftext_utils::FTextToString(c.locals + o) == kNoSpace) g_noSpaceSeen = true;
    return sg::Verdict::Run;
}

bool HintWatchLive() {
    if (!g_hintWatch) g_hintWatch = sg::WatchName(kRowHint, kTagHint, &OnHintPre, nullptr);
    if (!g_hintWatch) return false;
    sg::ResolvePendingNames();
    return sg::NameWatchLive(kRowHint, kTagHint);
}

void HintWatchOff() {
    if (!g_hintWatch) return;
    sg::UnwatchName(kRowHint, kTagHint, &OnHintPre, nullptr);
    g_hintWatch = false;
}

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

// The drill's one verdict, with its session in the reason; the hint watch is retired with it.
void Finish(const char* tag, coop::rig_verdict::Outcome outcome, const char* fmt, ...) {
    char why[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(why, sizeof(why), fmt, args);
    va_end(args);
    HintWatchOff();
    g_done = true;
    coop::rig_verdict::Say(tag, outcome, "session %d: %s", g_session, why);
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || g_done || !s || !s->connected() || !coop::players::Registry::Get().Local()) return;
    const bool host = s->role() == coop::net::Role::Host;
    const char* role = host ? "host" : "client";
    const char* tag = host ? "equip-host" : "equip-client";
    const uint64_t now = ::GetTickCount64();
    if (!g_startMs) g_startMs = now;
    // A wait on states nothing announces to this peer (its world's settling, the watch's resolve, a field the game writes
    // in place); each is a latched read per tick, and the bound says INVALID only.
    const bool ready = WorldReady(s) && HintWatchLive();
    const Slots before = ready ? Read() : Slots{};
    if (!ready || before.count < 0) {
        if (now - g_startMs > kReadyBoundMs)
            Finish(tag, coop::rig_verdict::Outcome::Invalid,
                   "the world never settled, the hint watch never went live or the equipment never read");
        return;
    }
    if (before.empty == 0) {
        Finish(tag, coop::rig_verdict::Outcome::Invalid, "none of the %d equipment slots is empty", before.count);
        return;
    }
    UE_LOGI("[EQUIP-DRILL] %s: %d slots, %d empty:%ls", role, before.count, before.empty, before.line.c_str());
    // Each offer while a slot stands empty. A refusal is the game's only when it said "Not enough space" with a slot
    // empty; a type or item already worn is the list's, and the next wearable is offered.
    for (const wchar_t* c : kWearables) {
        const Slots now0 = Read();
        if (now0.count < 0 || now0.empty == 0) break;
        g_noSpaceSeen = false;
        const bool ok = ue_wrap::begin_equipment::GiveFromClass(c);
        UE_LOGI("[EQUIP-DRILL] %s: equip %ls -> %s", role, c, ok ? "fit" : g_noSpaceSeen ? "refused, no space" : "refused");
        if (!ok && g_noSpaceSeen) {
            const Slots after = Read();
            UE_LOGI("[EQUIP-DRILL] %s: after the refusal, %d empty:%ls", role, after.empty, after.line.c_str());
            Finish(tag, coop::rig_verdict::Outcome::Fail,
                   "the game said \"Not enough space\" with %d of %d slots still empty", after.empty, before.count);
            return;
        }
    }
    const Slots after = Read();
    UE_LOGI("[EQUIP-DRILL] %s: after the list, %d empty:%ls", role, after.empty, after.line.c_str());
    if (after.empty == 0)
        Finish(tag, coop::rig_verdict::Outcome::Pass, "each of the %d empty slots took an item", before.empty);
    else
        Finish(tag, coop::rig_verdict::Outcome::Invalid,
               "the list ran out of wearables the game accepts with %d of %d slots still empty", after.empty,
               before.count);
}

void OnDisconnect() {
    if (!Enabled()) return;
    HintWatchOff();
    ++g_session;
    g_startMs = 0;
    g_done = false;
}

}  // namespace coop::dev::equip_drill
