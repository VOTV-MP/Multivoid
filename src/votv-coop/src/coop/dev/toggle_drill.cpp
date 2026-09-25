// coop/dev/toggle_drill.cpp -- see coop/dev/toggle_drill.h.

#include "coop/dev/toggle_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/interactable_sync.h"  // each lane's key
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/door_box.h"
#include "ue_wrap/devices/garage.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::toggle_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace A  = ue_wrap::appliance;
namespace DB = ue_wrap::door_box;
namespace G  = ue_wrap::garage;
namespace SW = ue_wrap::swinger;
using Clock = std::chrono::steady_clock;

// A step that waits on the other peer's toggle ends here: that toggle waits for its device to take
// one, then crosses in one reliable send at the verb.
constexpr auto kCrossBound = std::chrono::seconds(30);

// One kind of device: how it is picked, read and toggled, as its own lane keys and reads it.
struct Kind {
    const char* name;
    bool (*EnsureResolved)();
    bool (*Is)(void* obj);
    std::wstring (*Key)(void* obj);
    bool (*Read)(void* obj, bool& on);
    bool (*Ready)(void* obj);                  // may take a toggle now; null: always
    bool (*Toggle)(void* obj, void* player);   // the device's own verb
};

bool GarageAtRest(void* g) {
    bool moving = true;
    return G::TryReadMoving(g, moving) && !moving;
}
bool GarageToggle(void* g, void* p) { return G::CallRunTrigger(g, p, 0); }
bool TapToggle(void* a, void* p) { return A::CallAction(a, p, A::kTapToggleAction); }
bool LockerToggle(void* l, void* p) { return DB::CallAction(l, p, DB::kToggleAction); }
bool LidToggle(void* s, void*) {
    bool open = false;
    return SW::TryReadOpen(s, open) && (open ? SW::CallClose(s) : SW::CallOpen(s, false));
}

// Each device is keyed by its lane, whose name for it both peers share once both index it.
namespace IS = coop::interactable_sync;
constexpr Kind kKinds[] = {
    { "garage", &G::EnsureResolved, &G::IsGarage, &IS::GarageKey, &G::TryReadOpen, &GarageAtRest, &GarageToggle },
    { "tap", &A::EnsureResolved, &A::IsTap, &IS::ApplianceKey, &A::TryReadState, nullptr, &TapToggle },
    { "locker", &DB::EnsureResolved, &DB::IsLocker, &IS::DoorBoxKey, &DB::TryReadOpened, nullptr, &LockerToggle },
    { "lid", &SW::EnsureResolved, &SW::IsSwinger, &IS::ContainerKey, &SW::TryReadOpen, nullptr, &LidToggle },
};

enum class Phase { Unpicked, Waiting, Toggled, Done };

const Kind*  g_kind = nullptr;
void*        g_dev = nullptr;
int32_t      g_devIdx = -1;
std::wstring g_key;
int          g_start = -1;      // the device's state when this peer picked it
int          g_last = -1;       // its state at the last reading
bool         g_owed = false;    // this peer's toggle waits for its device to take one
bool         g_sawOwn = false;  // the client's copy has shown its own toggle
Phase        g_phase = Phase::Unpicked;
Clock::time_point g_since{};
Clock::time_point g_nextTry{};  // the next pick attempt while the lane names the devices
Clock::time_point g_nextSay{};  // the next line saying the pick still waits

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

// The host picks as soon as it hosts, so no toggle of a client's can land before its reading; a
// client once the host's snapshot is applied and its join is over.
bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The device both peers pick without telling each other: the one of the kind with the lowest key, the
// name its lane gives it. Sets `seen` to how many of the kind this world holds, named or not yet. One
// walk of the object array a try, for the drill only.
bool Pick(int& seen) {
    void* best = nullptr;
    int32_t bestIdx = -1;
    std::wstring bestKey;
    seen = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !g_kind->Is(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;  // the class default, no device
        ++seen;
        std::wstring key = g_kind->Key(o);
        if (key.empty() || key == L"None") continue;
        if (!best || key < bestKey) {
            best = o;
            bestIdx = R::InternalIndexOf(o);
            bestKey = std::move(key);
        }
    }
    g_dev = best;
    g_devIdx = bestIdx;
    g_key = bestKey;
    return best != nullptr;
}

// The owed toggle, once the device takes one, and its copy read straight after. Asked again next tick
// while it does not.
void TryOwedToggle() {
    if (g_kind->Ready && !g_kind->Ready(g_dev)) return;
    void* p = coop::players::Registry::Get().Local();
    const bool ran = p && g_kind->Toggle(g_dev, p);
    bool now = false;
    if (g_kind->Read(g_dev, now)) g_last = now ? 1 : 0;
    if (!coop::roster::LocalIsHost() && g_last != g_start) g_sawOwn = true;
    UE_LOGI("[TOGGLE-DRILL] %s TOGGLE %s key='%ls': dispatched=%d, state now %d", Side(), g_kind->name,
            g_key.c_str(), ran ? 1 : 0, g_last);
    g_owed = false;
    g_since = Clock::now();
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[TOGGLE-DRILL] %s DONE %s key='%ls' %s", Side(), g_kind ? g_kind->name : "?", g_key.c_str(), verdict);
}

// The kind named by toggle_drill, or null (off, or a name no kind has, said once).
const Kind* KindOf() {
    static const Kind* s_kind = [] () -> const Kind* {
        const std::string want = coop::config::ResolveString(::coop::config_registry::rows::toggle_drill);
        if (want.empty()) return nullptr;
        for (const Kind& k : kKinds)
            if (want == k.name) return &k;
        UE_LOGW("[TOGGLE-DRILL] toggle_drill='%s' names no kind (garage, tap, locker, lid) -- the drill is off",
                want.c_str());
        return nullptr;
    }();
    return s_kind;
}

}  // namespace

bool IsEnabled() { return KindOf() != nullptr; }

void Tick(coop::net::Session* session) {
    if (g_phase == Phase::Done || !IsEnabled()) return;
    g_kind = KindOf();
    if (!session || !session->connected() || !RoleIsReady() || !g_kind->EnsureResolved()) return;
    const bool host = coop::roster::LocalIsHost();
    if (g_phase == Phase::Unpicked) {
        if (Clock::now() < g_nextTry) return;
        int seen = 0;
        if (!Pick(seen)) {
            if (seen > 0) {  // its lane has not named them yet: asked again in a second, said every ten
                g_nextTry = Clock::now() + std::chrono::seconds(1);
                if (Clock::now() >= g_nextSay) {
                    g_nextSay = Clock::now() + std::chrono::seconds(10);
                    UE_LOGI("[TOGGLE-DRILL] %s: %d %s(s), none named by its lane yet", Side(), seen, g_kind->name);
                }
                return;
            }
            UE_LOGW("[TOGGLE-DRILL] %s: no %s in this world -- INCONCLUSIVE", Side(), g_kind->name);
            Done("none");
            return;
        }
        bool on = false;
        if (!g_kind->Read(g_dev, on)) {
            UE_LOGW("[TOGGLE-DRILL] %s: %s key='%ls' did not read -- INCONCLUSIVE", Side(), g_kind->name,
                    g_key.c_str());
            Done("unread");
            return;
        }
        g_start = g_last = on ? 1 : 0;
        g_phase = host ? Phase::Waiting : Phase::Toggled;
        g_owed = !host;
        g_since = Clock::now();
        UE_LOGI("[TOGGLE-DRILL] %s picked %s key='%ls', its state %d", Side(), g_kind->name, g_key.c_str(), g_start);
        return;
    }
    if (!R::IsLiveByIndex(g_dev, g_devIdx)) {
        UE_LOGW("[TOGGLE-DRILL] %s: %s key='%ls' is gone -- INCONCLUSIVE", Side(), g_kind->name, g_key.c_str());
        Done("gone");
        return;
    }
    bool on = false;
    if (!g_kind->Read(g_dev, on)) return;
    const int cur = on ? 1 : 0;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_since).count();
    if (cur != g_last) {
        UE_LOGI("[TOGGLE-DRILL] %s %s key='%ls' reads %d (was %d), %lld ms into this step", Side(), g_kind->name,
                g_key.c_str(), cur, g_last, ms);
        g_last = cur;
    }
    if (g_owed) {
        TryOwedToggle();
        return;
    }
    if (host && g_phase == Phase::Waiting) {
        // The client's toggle has crossed once this copy reads the other state; the host answers it and
        // goes on waiting, so a client that joins again drills again.
        if (cur != g_start) {
            UE_LOGI("[TOGGLE-DRILL] host SAW the client's toggle key='%ls' after %lld ms", g_key.c_str(), ms);
            g_owed = true;
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
            UE_LOGW("[TOGGLE-DRILL] client: %s within %lld s -- FAIL",
                    g_sawOwn ? "the host's toggle back did not cross" : "its own toggle never changed its copy",
                    static_cast<long long>(kCrossBound.count()));
            Done(g_sawOwn ? "crossed=0" : "own toggle did not run");
        }
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_dev = nullptr;
    g_devIdx = -1;
    g_key.clear();
    g_start = g_last = -1;
    g_owed = false;
    g_sawOwn = false;
    g_nextTry = g_nextSay = {};
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::toggle_drill
