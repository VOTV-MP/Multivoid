// coop/dev/appliance_drill.cpp -- see coop/dev/appliance_drill.h.

#include "coop/dev/appliance_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/interactable_sync.h"  // the appliance lane's key
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/appliance.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::appliance_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::appliance;
using Clock = std::chrono::steady_clock;

// A step that waits on the other peer's toggle ends here: a change that has not crossed by then did
// not. The client's toggle crosses the host's poll and one reliable send; the host's the same back.
constexpr auto kCrossBound = std::chrono::seconds(20);

enum class Phase { Unpicked, Waiting, Toggled, Done };

void*        g_faucet = nullptr;
int32_t      g_faucetIdx = -1;
std::wstring g_key;
int          g_start = -1;   // the faucet's state when this peer picked it
int          g_last = -1;    // its state at the last reading
bool         g_sawOwn = false;  // the client's copy has shown its own toggle
Phase        g_phase = Phase::Unpicked;
Clock::time_point g_since{};
Clock::time_point g_nextTry{};  // the next pick attempt while the lane indexes the faucets
Clock::time_point g_nextSay{};  // the next line saying the pick still waits

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

// The host picks as soon as it hosts, so no toggle of a client's can land before its reading; a
// client once the host's snapshot is applied and its join is over.
bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The faucet both peers pick without telling each other: the one with the lowest key in the
// appliance lane, the name both peers give it (a faucet the save does not key is named by its portable
// identity). Sets `faucets` to how many faucets this world holds, keyed or not yet. One walk of the
// object array a try, for the drill only.
bool PickFaucet(int& faucets, void*& firstFaucet) {
    void* best = nullptr;
    int32_t bestIdx = -1;
    std::wstring bestKey;
    faucets = 0;
    firstFaucet = nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !A::IsFaucet(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;  // the class default, no faucet
        if (++faucets == 1) firstFaucet = o;
        std::wstring key = coop::interactable_sync::ApplianceKey(o);
        if (key.empty()) continue;
        if (!best || key < bestKey) {
            best = o;
            bestIdx = R::InternalIndexOf(o);
            bestKey = std::move(key);
        }
    }
    g_faucet = best;
    g_faucetIdx = bestIdx;
    g_key = bestKey;
    return best != nullptr;
}

bool Toggle() {
    void* p = coop::players::Registry::Get().Local();
    return p && A::CallAction(g_faucet, p, A::kFaucetToggleAction);
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[APPL-DRILL] %s DONE key='%ls' %s", Side(), g_key.c_str(), verdict);
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::appliance_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady() || !A::EnsureResolved()) return;
    const bool host = coop::roster::LocalIsHost();
    if (g_phase == Phase::Unpicked) {
        if (Clock::now() < g_nextTry) return;
        int faucets = 0;
        void* first = nullptr;
        if (!PickFaucet(faucets, first)) {
            if (faucets > 0) {  // the lane has not indexed them yet: asked again in a second, said every ten
                g_nextTry = Clock::now() + std::chrono::seconds(1);
                if (Clock::now() >= g_nextSay) {
                    g_nextSay = Clock::now() + std::chrono::seconds(10);
                    UE_LOGI("[APPL-DRILL] %s: %d faucet(s), none named by the appliance lane yet; the first is %ls, "
                            "save key '%ls'", Side(), faucets, first ? R::ToString(R::NameOf(first)).c_str() : L"?",
                            first ? A::GetKeyString(first).c_str() : L"");
                }
                return;
            }
            UE_LOGW("[APPL-DRILL] %s: no faucet_C in this world -- INCONCLUSIVE", Side());
            Done("no faucet");
            return;
        }
        bool on = false;
        if (!A::TryReadState(g_faucet, on)) {
            UE_LOGW("[APPL-DRILL] %s: faucet key='%ls' did not read -- INCONCLUSIVE", Side(), g_key.c_str());
            Done("unread");
            return;
        }
        g_start = g_last = on ? 1 : 0;
        g_phase = Phase::Waiting;
        g_since = Clock::now();
        UE_LOGI("[APPL-DRILL] %s picked faucet key='%ls', its state %d", Side(), g_key.c_str(), g_start);
        if (!host) {
            const bool ran = Toggle();
            g_phase = Phase::Toggled;
            g_since = Clock::now();
            UE_LOGI("[APPL-DRILL] client TOGGLE key='%ls': dispatched=%d", g_key.c_str(), ran ? 1 : 0);
        }
        return;
    }
    if (!R::IsLiveByIndex(g_faucet, g_faucetIdx)) {
        UE_LOGW("[APPL-DRILL] %s: faucet key='%ls' is gone -- INCONCLUSIVE", Side(), g_key.c_str());
        Done("gone");
        return;
    }
    bool on = false;
    if (!A::TryReadState(g_faucet, on)) return;
    const int cur = on ? 1 : 0;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_since).count();
    if (cur != g_last) {
        UE_LOGI("[APPL-DRILL] %s key='%ls' reads %d (was %d), %lld ms into this step", Side(), g_key.c_str(),
                cur, g_last, ms);
        g_last = cur;
    }
    if (host && g_phase == Phase::Waiting) {
        // The client's toggle has crossed once this copy reads the other state; the host answers it.
        if (cur != g_start) {
            UE_LOGI("[APPL-DRILL] host SAW the client's toggle key='%ls' after %lld ms", g_key.c_str(), ms);
            const bool ran = Toggle();
            UE_LOGI("[APPL-DRILL] host TOGGLE back key='%ls': dispatched=%d", g_key.c_str(), ran ? 1 : 0);
            Done("the client's toggle crossed; toggled back");
        }
        return;  // no bound here: the host waits for however long the client's join takes
    }
    if (!host && g_phase == Phase::Toggled) {
        // First this copy shows its own toggle; then it reads the start state again once the host's
        // toggle back has crossed. A toggle that never showed here measured nothing.
        if (!g_sawOwn && cur != g_start) g_sawOwn = true;
        if (g_sawOwn && cur == g_start) {
            Done("crossed=1 both ways");
            return;
        }
        if (Clock::now() - g_since > kCrossBound) {
            UE_LOGW("[APPL-DRILL] client: %s within %lld s -- FAIL", g_sawOwn ? "the host's toggle back did not cross"
                    : "its own toggle never changed its copy", static_cast<long long>(kCrossBound.count()));
            Done(g_sawOwn ? "crossed=0" : "own toggle did not run");
        }
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_faucet = nullptr;
    g_faucetIdx = -1;
    g_key.clear();
    g_start = g_last = -1;
    g_sawOwn = false;
    g_nextTry = g_nextSay = {};
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::appliance_drill
