// coop/dev/prop_tool_park_selftest.cpp -- see coop/dev/prop_tool_park_selftest.h.

#include "coop/dev/prop_tool_park_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/props/prop_tool_park.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <vector>

namespace coop::dev::prop_tool_park_selftest {
namespace {

namespace PK = coop::prop_tool_park;
using coop::net::PropPropsStatePayload;

constexpr uint32_t kGoneEid = 9;   // the one element whose copy no longer exists

// The fake engine side: every element but kGoneEid has a copy (a distinct address per element), `g_held` says whether
// it is held, and the replays are recorded in order.
int  g_copies[16];
bool g_held = false;
struct Applied { uint32_t eid; uint8_t frozen; };
std::vector<Applied> g_applied;

void* Copy(uint32_t eid) { return eid == kGoneEid || eid >= 16 ? nullptr : &g_copies[eid]; }
bool  Held(void*) { return g_held; }
void  Apply(const PropPropsStatePayload& st, void*) { g_applied.push_back(Applied{st.eid, st.isFrozen}); }

constexpr PK::Env kEnv{&Copy, &Held, &Apply};

PropPropsStatePayload State(uint32_t eid, uint8_t frozen) {
    PropPropsStatePayload st{};
    st.eid = eid;
    st.isFrozen = frozen;
    return st;
}

struct Check {
    int pass = 0, total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("prop_tool_park selftest FAIL: %s", what);
    }
};

void Reset() {
    g_held = false;
    g_applied.clear();
}

void Run() {
    const bool breakIt = coop::config::ResolveFlag(coop::config_registry::rows::prop_props_park_selftest_break);
    Check check;

    // 1. The older state parked while the copy was held, a newer one reached the copy directly: the older is never
    // replayed over it.
    {
        Reset();
        PK::Ledger l;
        g_held = true;
        l.Park(State(3, 0), 0);
        g_held = false;
        if (!breakIt) l.Supersede(3);
        l.Retry(1, kEnv);
        check(g_applied.empty() && l.Empty(), "an older parked state was replayed over a newer direct one");
    }
    // 2. A parked state replays once, when the copy is let go, and is forgotten.
    {
        Reset();
        PK::Ledger l;
        g_held = true;
        l.Park(State(4, 1), 0);
        l.Retry(1, kEnv);
        check(g_applied.empty() && l.Has(4), "a state was replayed while its copy was still held");
        g_held = false;
        l.Retry(2, kEnv);
        l.Retry(3, kEnv);
        check(g_applied.size() == 1 && g_applied[0].eid == 4 && g_applied[0].frozen == 1 && l.Empty(),
              "a parked state did not replay exactly once at the release");
    }
    // 3. Two states parked for one element: the newer replaces the older.
    {
        Reset();
        PK::Ledger l;
        g_held = true;
        l.Park(State(5, 0), 0);
        l.Park(State(5, 1), 1);
        g_held = false;
        l.Retry(2, kEnv);
        check(g_applied.size() == 1 && g_applied[0].frozen == 1, "the older of two parked states was replayed");
    }
    // 4. A superseded element does not touch another element's parked state.
    {
        Reset();
        PK::Ledger l;
        g_held = true;
        l.Park(State(6, 1), 0);
        l.Park(State(7, 1), 0);
        l.Supersede(6);
        g_held = false;
        l.Retry(1, kEnv);
        check(g_applied.size() == 1 && g_applied[0].eid == 7, "a supersede of one element dropped another's state");
    }
    // 5. A copy that is gone is forgotten without a replay.
    {
        Reset();
        PK::Ledger l;
        l.Park(State(kGoneEid, 1), 0);
        l.Retry(1, kEnv);
        check(g_applied.empty() && l.Empty(), "a state for a gone copy was replayed or kept");
    }
    // 6. A copy held past the warning keeps its state, and the hold's end applies it.
    {
        Reset();
        PK::Ledger l;
        g_held = true;
        l.Park(State(8, 1), 0);
        l.Retry(PK::Ledger::kWarnMs + 1, kEnv);
        l.Retry(PK::Ledger::kWarnMs + 2, kEnv);
        check(g_applied.empty() && l.Has(8), "a state was dropped or replayed at the bound while its copy was held");
        g_held = false;
        l.Retry(PK::Ledger::kWarnMs + 3, kEnv);
        check(g_applied.size() == 1 && g_applied[0].eid == 8 && l.Empty(), "the hold's end did not apply the kept state");
    }
    // 7. Clear forgets every parked state.
    {
        Reset();
        PK::Ledger l;
        g_held = true;
        l.Park(State(10, 1), 0);
        l.Clear();
        g_held = false;
        l.Retry(1, kEnv);
        check(g_applied.empty() && l.Empty(), "Clear left a parked state");
    }

    if (check.pass == check.total) {
        UE_LOGI("prop_tool_park selftest: ALL PASS (%d checks)", check.total);
        return;
    }
    UE_LOGE("prop_tool_park selftest: %d/%d checks passed", check.pass, check.total);
}

bool g_ran = false;

}  // namespace

void Tick() {
    if (g_ran) return;
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::prop_props_park_selftest);
    if (!on) {
        g_ran = true;
        return;
    }
    g_ran = true;
    Run();
}

}  // namespace coop::dev::prop_tool_park_selftest
