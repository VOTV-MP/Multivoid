// coop/dev/light_drill.cpp -- see coop/dev/light_drill.h.

#include "coop/dev/light_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/interactable_sync.h"  // the light switch lane's key
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/lightswitch.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::light_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace LS = ue_wrap::lightswitch;
using Clock = std::chrono::steady_clock;

// A step that waits on the other peer's change ends here: one that has not crossed by then did not.
constexpr auto kCrossBound = std::chrono::seconds(20);

enum class Phase { Unpicked, Waiting, Pressed, SawHost, Done };

void*        g_switch = nullptr;
int32_t      g_switchIdx = -1;
void*        g_group = nullptr;
std::wstring g_key;          // the switch's save key
int          g_start = -1;   // the group's state when this peer picked it
int          g_last = -1;    // its state at the last reading
Phase        g_phase = Phase::Unpicked;
Clock::time_point g_since{};
Clock::time_point g_nextTry{};  // the next pick attempt while the lane indexes the switches

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

// The host picks as soon as it hosts, so no press of a client's lands before its reading; a client
// once the host's snapshot is applied and its join is over.
bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

int ReadGroup() {
    bool on = false;
    return (g_group && LS::TryReadActive(g_group, on)) ? (on ? 1 : 0) : -1;
}

// The switch both peers pick without telling each other: the lowest key in the switch lane, the name
// both peers give it, among switches whose group resolves and has its breaker on, so a press can
// toggle it. Sets `switches` to how many such switches this world holds, keyed or not yet. One walk
// of the object array a try, for the drill only.
bool PickSwitch(int& switches) {
    void* best = nullptr;
    int32_t bestIdx = -1;
    void* bestGroup = nullptr;
    std::wstring bestKey;
    switches = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !LS::IsLightSwitch(o)) continue;
        void* group = LS::SwitchRoot(o);
        bool breaker = false;
        if (!group || !LS::TryReadBreaker(group, breaker) || !breaker) continue;
        ++switches;
        std::wstring key = coop::interactable_sync::LightSwitchKey(o);
        if (key.empty()) continue;
        if (!best || key < bestKey) {
            best = o;
            bestIdx = R::InternalIndexOf(o);
            bestGroup = group;
            bestKey = std::move(key);
        }
    }
    g_switch = best;
    g_switchIdx = bestIdx;
    g_group = bestGroup;
    g_key = bestKey;
    return best != nullptr;
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[LIGHT-DRILL] %s DONE switch='%ls' %s", Side(), g_key.c_str(), verdict);
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::light_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (!LS::EnsureResolved() || !LS::EnsureSwitchResolved()) return;
    const bool host = coop::roster::LocalIsHost();
    if (g_phase == Phase::Unpicked) {
        if (Clock::now() < g_nextTry) return;
        int switches = 0;
        if (!PickSwitch(switches)) {
            if (switches > 0) {  // the lane has not indexed them yet: asked again in a second
                g_nextTry = Clock::now() + std::chrono::seconds(1);
                return;
            }
            UE_LOGW("[LIGHT-DRILL] %s: no switch with a powered group in this world -- INCONCLUSIVE", Side());
            Done("no switch");
            return;
        }
        g_start = g_last = ReadGroup();
        g_phase = Phase::Waiting;
        g_since = Clock::now();
        UE_LOGI("[LIGHT-DRILL] %s picked switch='%ls', its group reads %d", Side(), g_key.c_str(), g_start);
        if (!host) {
            bool aBefore = false, aAfter = false;
            LS::TryReadSwitchA(g_switch, aBefore);
            const bool ran = LS::CallUse(g_switch);
            LS::TryReadSwitchA(g_switch, aAfter);
            const int groupNow = ReadGroup();
            g_phase = Phase::Pressed;
            g_since = Clock::now();
            // The press runs use() here in full: the switch flips, and the group's runTrigger it ends in
            // is refused on this copy, so the group reads as it did until the host's state arrives.
            UE_LOGI("[LIGHT-DRILL] client PRESS switch='%ls': dispatched=%d, switch %d -> %d, own group %d -> %d "
                    "(%s)", g_key.c_str(), ran ? 1 : 0, aBefore ? 1 : 0, aAfter ? 1 : 0, g_start, groupNow,
                    groupNow == g_start ? "left, as it should" : "MOVED LOCALLY");
            if (groupNow != g_start) Done("the press moved this copy's group itself -- FAIL");
        }
        return;
    }
    if (!R::IsLiveByIndex(g_switch, g_switchIdx)) {
        UE_LOGW("[LIGHT-DRILL] %s: switch='%ls' is gone -- INCONCLUSIVE", Side(), g_key.c_str());
        Done("gone");
        return;
    }
    const int cur = ReadGroup();
    if (cur < 0) return;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_since).count();
    if (cur != g_last) {
        UE_LOGI("[LIGHT-DRILL] %s switch='%ls' group reads %d (was %d), %lld ms into this step", Side(),
                g_key.c_str(), cur, g_last, ms);
        g_last = cur;
    }
    if (host && g_phase == Phase::Waiting) {
        // The client's press has crossed once this group reads the other state; the host answers it.
        if (cur != g_start) {
            UE_LOGI("[LIGHT-DRILL] host SAW the client's press move the group after %lld ms", ms);
            const bool ran = LS::CallUse(g_switch);
            UE_LOGI("[LIGHT-DRILL] host PRESS switch='%ls': dispatched=%d, group now %d", g_key.c_str(),
                    ran ? 1 : 0, ReadGroup());
            Done("the client's press crossed; pressed back");
        }
        return;  // no bound here: the host waits for however long the client's join takes
    }
    if (!host && (g_phase == Phase::Pressed || g_phase == Phase::SawHost)) {
        if (g_phase == Phase::Pressed && cur != g_start) {
            g_phase = Phase::SawHost;
            g_since = Clock::now();
            UE_LOGI("[LIGHT-DRILL] client SAW the host's group change after %lld ms", ms);
            return;
        }
        if (g_phase == Phase::SawHost && cur == g_start) {
            Done("crossed=1 both ways");
            return;
        }
        if (Clock::now() - g_since > kCrossBound) {
            UE_LOGW("[LIGHT-DRILL] client: %s within %lld s -- FAIL", g_phase == Phase::Pressed
                    ? "the host's group change never arrived" : "the host's own press never crossed",
                    static_cast<long long>(kCrossBound.count()));
            Done(g_phase == Phase::Pressed ? "crossed=0" : "crossed=1 one way");
        }
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_switch = nullptr;
    g_switchIdx = -1;
    g_group = nullptr;
    g_key.clear();
    g_start = g_last = -1;
    g_nextTry = {};
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::light_drill
