// coop/dev/toggle_drill.cpp -- see coop/dev/toggle_drill.h.

#include "coop/dev/toggle_drill.h"

#include "coop/config/config.h"
#include "coop/element/portable_identity.h"  // a pick both peers make alike
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
// A kind whose devices this world holds but whose lane names none within this, is said and left.
constexpr auto kNameBound = std::chrono::seconds(60);

// One kind of device: how it is picked, read and toggled, as its own lane keys and reads it. A
// one-way kind's state goes 0 to 1 once (a repair): one peer makes the change and the other reads it
// cross, and the host's fixture, before any client connects, puts back a device its save had changed,
// so the joiner's world (the host's, captured live at the join) starts from 0 too.
struct Kind {
    const char* name;
    bool (*EnsureResolved)();
    bool (*Is)(void* obj);
    std::wstring (*Key)(void* obj);
    bool (*Read)(void* obj, bool& on);
    bool (*Ready)(void* obj);                  // may take a toggle now; null: always
    bool (*Toggle)(void* obj, void* player);   // the device's own verb
    bool oneWay = false;
    bool hostAuthors = false;                  // one way: the host makes the change, else the client
    bool (*Break)(void* obj) = nullptr;        // one way: the host's fixture
};

bool GarageAtRest(void* g) {
    bool moving = true;
    return G::TryReadMoving(g, moving) && !moving;
}
bool GarageToggle(void* g, void* p) { return G::CallRunTrigger(g, p, 0); }
bool TapToggle(void* a, void* p) { return A::CallAction(a, p, A::kTapToggleAction); }
bool LockerToggle(void* l, void* p) { return DB::CallAction(l, p, DB::kToggleAction); }
// A lid that cannot lock: no refused open, and no tick that closes it at rest on its own.
bool IsSteadyLid(void* s) {
    bool lockable = true;
    return SW::IsSwinger(s) && SW::TryReadLockable(s, lockable) && !lockable;
}
bool LidToggle(void* s, void*) {
    bool open = false;
    return SW::TryReadOpen(s, open) && (open ? SW::CallClose(s) : SW::CallOpen(s, false));
}
bool OvenRepair(void* o, void*) { return A::CallOvenFix(o); }
bool OvenBreak(void* o) { return A::WriteOvenFixed(o, false); }

// Each device is keyed by its lane, whose name for it both peers share once both index it.
namespace IS = coop::interactable_sync;
constexpr Kind kKinds[] = {
    { "garage", &G::EnsureResolved, &G::IsGarage, &IS::GarageKey, &G::TryReadOpen, &GarageAtRest, &GarageToggle },
    { "tap", &A::EnsureResolved, &A::IsTap, &IS::ApplianceKey, &A::TryReadState, nullptr, &TapToggle },
    { "locker", &DB::EnsureResolved, &DB::IsLocker, &IS::DoorBoxKey, &DB::TryReadOpened, nullptr, &LockerToggle },
    { "lid", &SW::EnsureResolved, &IsSteadyLid, &IS::ContainerKey, &SW::TryReadOpen, nullptr, &LidToggle },
    { "oven", &A::EnsureResolved, &A::IsOven, &IS::OvenKey, &A::TryReadOvenFixed, nullptr, &OvenRepair,
      true, false, &OvenBreak },
    { "oven_host", &A::EnsureResolved, &A::IsOven, &IS::OvenKey, &A::TryReadOvenFixed, nullptr, &OvenRepair,
      true, true, &OvenBreak },
};

enum class Phase { Unpicked, Waiting, Toggled, Done };

const Kind*  g_kind = nullptr;
void*        g_dev = nullptr;
int32_t      g_devIdx = -1;
std::wstring g_key;
int          g_start = -1;      // the device's state when this peer picked it
int          g_last = -1;       // its state at the last reading
bool         g_owed = false;    // this peer's toggle waits for its device to take one
Clock::time_point g_owedSince{};  // since when
bool         g_sawOwn = false;  // the client's copy has shown its own toggle
Phase        g_phase = Phase::Unpicked;
Clock::time_point g_since{};
Clock::time_point g_nextTry{};  // the next pick attempt while the lane names the devices
Clock::time_point g_nextSay{};  // the next line saying the pick still waits
Clock::time_point g_pickSince{};  // the first pick attempt
bool         g_fixtured = false;  // a one-way kind's host fixture has run (or can no longer)

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

// The host picks as soon as it hosts, so no toggle of a client's can land before its reading; a
// client once the host's snapshot is applied and its join is over. A client reading a host's one-way
// change picks at its world's announce instead, before the host's change can reach it.
bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    if (!coop::net_pump::HasAnnouncedWorldReady()) return false;
    return (g_kind->oneWay && g_kind->hostAuthors) ||
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
        // Only a device both peers name alike: a game key minted per process sorts anywhere.
        if (coop::element::PortableWireKey(o).empty()) continue;
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

// A one-way kind's fixture, on the host before any client connects: the device the drill picks, put
// back where the save had changed it. Once the lane names it; said either way.
void TryFixture() {
    if (Clock::now() < g_nextTry) return;
    int seen = 0;
    if (!Pick(seen)) {
        g_nextTry = Clock::now() + std::chrono::seconds(1);
        return;
    }
    bool on = false;
    if (!g_kind->Read(g_dev, on) || !on) {
        UE_LOGI("[TOGGLE-DRILL] host FIXTURE %s key='%ls': 0 in the save, nothing to put back", g_kind->name,
                g_key.c_str());
    } else {
        const bool ran = g_kind->Break(g_dev);
        const bool read = g_kind->Read(g_dev, on);
        UE_LOGI("[TOGGLE-DRILL] host FIXTURE %s key='%ls': 1 in the save, put back ran=%d, reads %d", g_kind->name,
                g_key.c_str(), ran ? 1 : 0, read ? (on ? 1 : 0) : -1);
    }
    g_fixtured = true;
    g_nextTry = {};
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[TOGGLE-DRILL] %s DONE %s key='%ls' %s", Side(), g_kind ? g_kind->name : "?", g_key.c_str(), verdict);
}

void Owe() {
    g_owed = true;
    g_owedSince = Clock::now();
}

// The owed toggle, once the device takes one, and its copy read straight after. Asked again next tick
// while it does not, up to the bound; a toggle that did not dispatch ends the run.
void TryOwedToggle() {
    if (g_kind->Ready && !g_kind->Ready(g_dev)) {
        if (Clock::now() - g_owedSince <= kCrossBound) return;
        UE_LOGW("[TOGGLE-DRILL] %s: the %s took no toggle within %lld s (never ready) -- FAIL", Side(),
                g_kind->name, static_cast<long long>(kCrossBound.count()));
        Done("never ready");
        return;
    }
    void* p = coop::players::Registry::Get().Local();
    const int before = g_last;
    const bool ran = p && g_kind->Toggle(g_dev, p);
    bool now = false;
    if (g_kind->Read(g_dev, now)) g_last = now ? 1 : 0;
    if (!coop::roster::LocalIsHost() && g_last != g_start) g_sawOwn = true;
    UE_LOGI("[TOGGLE-DRILL] %s TOGGLE %s key='%ls': dispatched=%d, state now %d", Side(), g_kind->name,
            g_key.c_str(), ran ? 1 : 0, g_last);
    g_owed = false;
    g_since = Clock::now();
    if (!ran) Done("the toggle did not dispatch -- FAIL");
    else if (g_last == before) Done("the device took the toggle and did not move (refused) -- INCONCLUSIVE");
    else if (g_kind->oneWay)
        UE_LOGI("[TOGGLE-DRILL] %s CHANGED %s key='%ls' one way; the other peer reads it cross", Side(),
                g_kind->name, g_key.c_str());
}

// The kind named by toggle_drill, or null (off, or a name no kind has, said once).
const Kind* KindOf() {
    static const Kind* s_kind = [] () -> const Kind* {
        const std::string want = coop::config::ResolveString(::coop::config_registry::rows::toggle_drill);
        if (want.empty()) return nullptr;
        for (const Kind& k : kKinds)
            if (want == k.name) return &k;
        UE_LOGW("[TOGGLE-DRILL] toggle_drill='%s' names no kind (garage, tap, locker, lid, oven, oven_host) -- "
                "the drill is off",
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
    if (!session) return;
    if (!g_kind->EnsureResolved()) {
        // A kind whose classes never load in this world measures nothing: said once the peer is ready
        // and the naming bound has passed.
        if (session->connected() && RoleIsReady()) {
            if (g_pickSince == Clock::time_point{}) {
                g_pickSince = Clock::now();
            } else if (Clock::now() - g_pickSince > kNameBound) {
                UE_LOGW("[TOGGLE-DRILL] %s: no class of the %s kind loaded here -- INCONCLUSIVE", Side(),
                        g_kind->name);
                Done("none");
            }
        }
        return;
    }
    if (g_kind->oneWay && !g_fixtured && session->role() == coop::net::Role::Host) {
        if (!session->connected()) {
            TryFixture();
            return;
        }
        g_fixtured = true;  // a client came first: the reading side judges a device that starts at 1
        UE_LOGW("[TOGGLE-DRILL] host: a client connected before the fixture ran");
    }
    if (!session->connected() || !RoleIsReady()) return;
    const bool host = coop::roster::LocalIsHost();
    if (g_phase == Phase::Unpicked) {
        if (g_pickSince == Clock::time_point{}) g_pickSince = Clock::now();
        if (Clock::now() < g_nextTry) return;
        int seen = 0;
        if (!Pick(seen)) {
            if (seen > 0 && Clock::now() - g_pickSince <= kNameBound) {
                // Its lane has not named them yet: asked again in a second, said every ten.
                g_nextTry = Clock::now() + std::chrono::seconds(1);
                if (Clock::now() >= g_nextSay) {
                    g_nextSay = Clock::now() + std::chrono::seconds(10);
                    UE_LOGI("[TOGGLE-DRILL] %s: %d %s(s), none named by its lane yet", Side(), seen, g_kind->name);
                }
                return;
            }
            UE_LOGW("[TOGGLE-DRILL] %s: %s -- INCONCLUSIVE", Side(), seen > 0
                    ? "its lane named none of them within the bound" : "none of the kind in this world");
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
        const bool author = !g_kind->oneWay || host == g_kind->hostAuthors;
        g_phase = (host && !g_kind->oneWay) || !author ? Phase::Waiting : Phase::Toggled;
        if (!host && author) Owe();
        g_since = Clock::now();
        UE_LOGI("[TOGGLE-DRILL] %s picked %s key='%ls', its state %d", Side(), g_kind->name, g_key.c_str(), g_start);
        if (g_kind->oneWay && g_start != 0) Done("it starts at 1, a one-way change cannot show -- INCONCLUSIVE");
        return;
    }
    if (!R::IsLiveByIndex(g_dev, g_devIdx)) {
        UE_LOGW("[TOGGLE-DRILL] %s: %s key='%ls' is gone -- INCONCLUSIVE", Side(), g_kind->name, g_key.c_str());
        Done("gone");
        return;
    }
    bool on = false;
    if (!g_kind->Read(g_dev, on)) {
        if (Clock::now() - g_since > kCrossBound) {
            UE_LOGW("[TOGGLE-DRILL] %s: %s key='%ls' has not read for %lld s -- INCONCLUSIVE", Side(), g_kind->name,
                    g_key.c_str(), static_cast<long long>(kCrossBound.count()));
            Done("unread");
        }
        return;
    }
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
    if (g_kind->oneWay) {
        if (g_phase == Phase::Waiting) {
            // The reader: its copy shows the other peer's change once it crossed.
            if (cur != g_start) {
                UE_LOGI("[TOGGLE-DRILL] %s SAW the %s's change key='%ls' after %lld ms", Side(),
                        host ? "client" : "host", g_key.c_str(), ms);
                Done("crossed=1 one way");
            } else if (!host && Clock::now() - g_since > kCrossBound) {
                UE_LOGW("[TOGGLE-DRILL] client: the host's change did not cross within %lld s -- FAIL",
                        static_cast<long long>(kCrossBound.count()));
                Done("crossed=0");
            }
        } else if (host && g_last == g_start && session->AnyWorldReadyPeer()) {
            Owe();  // the host's change, once a client's world is there to take it
        }
        return;  // the author waits; the reader ends the run
    }
    if (host && g_phase == Phase::Waiting) {
        // The client's toggle has crossed once this copy reads the other state; the host answers it and
        // goes on waiting, so a client that joins again drills again.
        if (cur != g_start) {
            UE_LOGI("[TOGGLE-DRILL] host SAW the client's toggle key='%ls' after %lld ms", g_key.c_str(), ms);
            Owe();
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
    g_nextTry = g_nextSay = g_pickSince = {};
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::toggle_drill
