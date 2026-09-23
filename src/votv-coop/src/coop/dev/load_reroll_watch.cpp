// coop/dev/load_reroll_watch.cpp -- see coop/dev/load_reroll_watch.h.

#include "coop/dev/load_reroll_watch.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/coord_tower.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <string>

namespace coop::dev::load_reroll_watch {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace WI = ue_wrap::world_identity;

// Each name has one declaring class in the cooked game (a census of every cooked Blueprint,
// compared case-insensitively as the engine compares names).
struct WatchDef { const wchar_t* name; const char* declarer; };
constexpr WatchDef kWatches[] = {
    { L"upgrades",            "initialServerUpgradeSpawn_C" },
    { L"Scramble Radar Dish", "coordRadarDish_C" },
};
constexpr int kCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
constexpr int kTagBase = 0x4C520000;  // 'LR', plus the watch's index
constexpr int kMaxEntryLines = 32;    // the tower's own timer can call it for the whole session

// Game thread only: the gate fires on it, and the pump ticks on it.
char     g_role = '-';               // H, C, or '-' before a session runs
bool     g_registered = false;
bool     g_registerFailed = false;
int      g_live = -1;
uint64_t g_reached[kCount] = {};
uint64_t g_ran[kCount] = {};
int      g_entryLines = 0;
bool     g_ticked = false;           // a previous tick exists, and it left the switch on
int      g_foundOff = 0;             // ticks that found the switch off
uint32_t g_worldGen = 0;

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::load_reroll_watch);
    return s;
}

char RoleChar() { return g_role; }

sg::Verdict OnPre(const sg::Call& c) {
    const int i = c.tag - kTagBase;
    if (i < 0 || i >= kCount) return sg::Verdict::Run;
    ++g_reached[i];
    if (g_entryLines >= kMaxEntryLines) return sg::Verdict::Run;
    ++g_entryLines;
    const std::wstring fn = c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : L"<ProcessEvent>";
    const std::wstring cls = c.callerObject ? R::ClassNameOf(c.callerObject) : L"-";
    const std::wstring inst = c.object ? R::ClassNameOf(c.object) : L"?";
    const int32_t towerId = (i == 1) ? ue_wrap::coord_tower::IdOf(c.object) : -1;  // the tower's own watch only
    UE_LOGI("load_reroll_watch: [%c] ENTRY %ls on a %ls (tower id %d) -- from %ls::%ls%s | join phase %s, "
            "world generation %u",
            RoleChar(), kWatches[i].name, inst.c_str(), towerId, cls.c_str(),
            fn.c_str(), c.fromOurCode ? " (a reflected call of ours)" : "",
            coop::join_progress::PhaseName(coop::join_progress::CurrentPhase()), WI::Generation());
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& c) {
    const int i = c.tag - kTagBase;
    if (i >= 0 && i < kCount) ++g_ran[i];
}

// Registered once per process; a name watch holds its slots for good.
void EnsureWatches() {
    if (!g_registered && !g_registerFailed) {
        if (!sg::IsInstalled()) return;
        for (int i = 0; i < kCount; ++i) {
            if (!sg::WatchName(kWatches[i].name, kTagBase + i, &OnPre, &OnPost)) {
                g_registerFailed = true;
                UE_LOGE("load_reroll_watch: the gate refused the watch on %ls -- this run measures nothing",
                        kWatches[i].name);
                return;
            }
        }
        g_registered = true;
    }
    if (!g_registered || g_live == kCount) return;
    sg::ResolvePendingNames();
    int live = 0;
    for (int i = 0; i < kCount; ++i)
        if (sg::NameWatchLive(kWatches[i].name, kTagBase + i)) ++live;
    if (live == g_live) return;
    g_live = live;
    UE_LOGI("load_reroll_watch: [%c] %d of %d name watches LIVE (%ls on %s, %ls on %s)", RoleChar(), live, kCount,
            kWatches[0].name, kWatches[0].declarer, kWatches[1].name, kWatches[1].declarer);
}

}  // namespace

void Tick(const coop::net::Session& session) {
    if (!IsEnabled()) return;
    // The role a session was configured with is meaningless until it runs.
    g_role = !session.running() ? '-' : session.role() == coop::net::Role::Host ? 'H' : 'C';
    // Every tick leaves the switch on, so a tick that finds it OFF means something turned it off
    // inside the interval since the previous one -- the interval a load between them ran in.
    const bool found = sg::IsEnabled();
    if (!found) {
        if (++g_foundOff <= 16)
            UE_LOGI("load_reroll_watch: [%c] found the gate switch OFF (#%d); turned back on", RoleChar(), g_foundOff);
        sg::SetEnabled(true);
    }
    EnsureWatches();
    const uint32_t gen = WI::Generation();
    if (gen != g_worldGen) {
        UE_LOGI("load_reroll_watch: [%c] WORLD generation %u -> %u | the gate switch %s, found %s by this tick | "
                "watches live %d/%d | so far: upgrades reached %llu ran %llu, Scramble Radar Dish reached %llu ran "
                "%llu | join phase %s",
                RoleChar(), g_worldGen, gen, g_ticked ? "left on by the previous tick" : "(first tick)",
                found ? "on" : "OFF", g_live < 0 ? 0 : g_live, kCount,
                static_cast<unsigned long long>(g_reached[0]), static_cast<unsigned long long>(g_ran[0]),
                static_cast<unsigned long long>(g_reached[1]), static_cast<unsigned long long>(g_ran[1]),
                coop::join_progress::PhaseName(coop::join_progress::CurrentPhase()));
        g_worldGen = gen;
    }
    g_ticked = true;
}

}  // namespace coop::dev::load_reroll_watch
