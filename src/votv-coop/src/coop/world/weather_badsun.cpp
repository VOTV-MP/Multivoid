// coop/world/weather_badsun.cpp -- see coop/world/weather_badsun.h.

#include "coop/world/weather_badsun.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/bad_sun.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::weather_badsun {
namespace {

namespace BS = ue_wrap::bad_sun;
namespace GT = ue_wrap::game_thread;
namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSpawnBadSun, "weather_badsun", L"mainGamemode_C", L"Spawn Bad Sun", Declared);
UE_FACT_FN(kRowRemove, "weather_badsun", L"badSun_C", L"remove", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

// Game thread, but for the session pointer.
constexpr int kSpawnTag = 0x42535350;   // 'BSSP'
constexpr int kRemoveTag = 0x42535245;  // 'BSRE'
bool g_gateAsked = false;     // the watches were registered (or refused, said)
bool g_applying = false;      // the lane's own apply of the host's Bad Sun runs
// The one payload that met no gamemode, no day cycle or no drainable index, the newest: a client's, applied from
// Tick once the world can take it. `g_keptSaid`: the reason of the last kept line, said once per reason a payload
// waits on (-1 = nothing said; cleared when an apply counts the copies).
enum class Until { World, Index };
bool g_hasPending = false;
coop::net::BadSunPayload g_pending{};
int g_keptSaid = -1;
bool g_saidCycleUnreadable = false;

// One name watch and what Tick has said of it. `said`: its state is final and was logged, or the gate refused it
// and said so. `settled`: the gate's name resolve finished (live, or dead for good). A settled watch is final once
// its class has loaded, because the gate asks the class then and can still kill the watch.
struct Watch {
    const ue_wrap::facts::Function* row;
    int tag;
    const wchar_t* cls;
    const char* what;
    const char* consequence;
    bool said = false;
    bool settled = false;
};
Watch g_spawnWatch{&kRowSpawnBadSun, kSpawnTag, L"mainGamemode_C", "Spawn Bad Sun",
                   "the host's fresh Bad Sun sends nothing, and a client's own Spawn Bad Sun runs"};
Watch g_removeWatch{&kRowRemove, kRemoveTag, L"badSun_C", "remove",
                    "client copies still end by their clocks; the host's removal sends nothing"};
bool g_seenVersionValid = false;
uint64_t g_seenVersion = 0;   // the class set's version at Tick's last pass
// This session's counts, said at its end.
uint64_t g_refused = 0;       // a client's own `Spawn Bad Sun` refused
bool     g_saidRefused = false;
uint64_t g_sent = 0;          // the host's state sent as a call left it
uint64_t g_spawned = 0;       // the host's Bad Sun spawned here
uint64_t g_destroyed = 0;     // an inactive copy destroyed here
uint64_t g_leftActive = 0;    // an active copy left to end by its own clock
uint64_t g_applyFailed = 0;   // an apply that did not leave this peer as the host's state
uint64_t g_sentAbsent = 0;    // the host's "no Bad Sun" sent
uint64_t g_removesRan = 0;    // a copy's own remove() ran on this peer
bool g_lastApplied = false, g_lastWant = false, g_lastSpawned = false;  // the newest apply that counted the copies
int  g_lastLive = 0;

// The lane's own apply, for the length of one spawn: the gate lets exactly this call run.
struct Applying {
    Applying() { g_applying = true; }
    ~Applying() { g_applying = false; }
    Applying(const Applying&) = delete;
    Applying& operator=(const Applying&) = delete;
};

bool IsHost(coop::net::Session* s) { return s && s->connected() && s->role() == coop::net::Role::Host; }

// A kept payload and an apply are a client's: no session, or the host's, has nothing to hold or to apply.
bool IsClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() != coop::net::Role::Host;
}

// This world's day cycle, the world a copy can run in. `Unreadable`: the gamemode's member did not resolve (said once
// per session); the payload then waits like any other.
enum class Cycle { Absent, Present, Unreadable };
Cycle ReadCycle() {
    bool has = false;
    if (!BS::ReadHasCycle(has)) {
        if (!g_saidCycleUnreadable) {
            g_saidCycleUnreadable = true;
            UE_LOGW("weather: the gamemode's day cycle member did not resolve -- a Bad Sun payload is kept and none "
                    "applies on this peer");
        }
        return Cycle::Unreadable;
    }
    return has ? Cycle::Present : Cycle::Absent;
}

// CLIENT, before the gamemode's `Spawn Bad Sun`: the host's state owns whether a copy exists, and only the lane's
// apply runs. No world's load calls this verb (a saved copy comes back with the world), so no clause for a world
// this client has not announced ready. Leaves MTA's shape: its client runs its own setWeather, local only
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:6954-6965); here the verb is
// cancelled, since a copy a client made would act on its own poll with the host unaware.
sg::Verdict OnSpawnPre(const sg::Call& /*call*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    if (g_applying) return sg::Verdict::Run;
    ++g_refused;
    if (!g_saidRefused) {
        g_saidRefused = true;
        UE_LOGI("weather: a client refused its own Spawn Bad Sun -- the host's state owns whether a Bad Sun exists "
                "(first refusal; the rest are counted)");
    }
    return sg::Verdict::Cancel;
}

// HOST: the state a call left, to every world-ready client. `what` names the call for the log.
void SendState(coop::net::Session* s, bool present, const char* what) {
    if (!s->AnyWorldReadyPeer()) {
        UE_LOGI("weather: the host's %s ran -- Bad Sun %d; no client's world is ready, the seed carries it", what,
                present ? 1 : 0);
        return;
    }
    coop::net::BadSunPayload p{};
    const coop::element::ElementId self = coop::players::Registry::Get().LocalPlayerElementId();
    p.senderElementId = self == coop::element::kInvalidId ? 0u : self;
    p.state = present ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::BadSun, &p, sizeof(p))) {
        UE_LOGW("weather: the host's Bad Sun %d was not sent -- SendReliable failed", present ? 1 : 0);
        return;
    }
    ++g_sent;
    if (!present) ++g_sentAbsent;
    UE_LOGI("weather: the host's %s ran -- sent Bad Sun %d", what, present ? 1 : 0);
}

// HOST, after `Spawn Bad Sun` ran: the field the verb just wrote.
void OnSpawnPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s)) return;
    bool present = false;
    if (!BS::ReadGamemodeCopy(call.object, present)) {
        UE_LOGW("weather: the host's Spawn Bad Sun ran and the gamemode's copy did not read -- nothing sent");
        return;
    }
    SendState(s, present, "Spawn Bad Sun");
}

// EVERY peer, after a copy's own remove() ran; on the host also the end the clients are told. Nothing else sends
// absent: a world's teardown or a session's end is no edge.
void OnRemovePost(const sg::Call& /*call*/) {
    ++g_removesRan;
    UE_LOGI("weather: this copy's own remove() ran");
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s)) return;
    SendState(s, false, "remove()");
}

void FailedApply(const char* why, bool want) {
    if (g_applyFailed++ == 0)
        UE_LOGW("weather: the host's Bad Sun %d did not apply -- %s (first failure; the rest are counted, and the "
                "next edge tries again)", want ? 1 : 0, why);
}

// One copy's state, for the log.
const char* StateOf(void* copy) {
    bool active = false;
    if (!BS::ReadActive(copy, active)) return "unreadable";
    return active ? "active" : "inactive";
}

// One watch's final state, said once. A settled live watch is not final until its class has loaded; until then it
// is asked again only when the class set moved (`classMoved`), or at once when it has just settled.
void SayWatch(Watch& w, bool classMoved) {
    if (w.said) return;
    bool justSettled = false;
    if (!w.settled) {
        if (!sg::NameWatchSettled(*w.row, w.tag)) return;
        w.settled = true;
        justSettled = true;
    }
    if (!justSettled && !classMoved) return;
    // The class first: a settled watch with its class unloaded cannot change state, so the name table is not walked.
    const bool loaded = ue_wrap::object_index::ClassByName(w.cls) != nullptr;
    if (!justSettled && !loaded) return;
    const bool live = sg::NameWatchLive(*w.row, w.tag);
    if (live && !loaded) return;
    w.said = true;
    if (live)
        UE_LOGI("weather: the %s watch is live", w.what);
    else
        UE_LOGE("weather: the %s watch is dead -- %s", w.what, w.consequence);
}

// The host's payload that waited: the lane's own apply, now that this world can take it. MTA's client holds the
// received weather in one model that every packet overwrites
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:2259-2265); the kept slot is that model,
// drained from Tick and not read continuously.
void Keep(const coop::net::BadSunPayload& payload, Until until) {
    g_pending = payload;
    g_hasPending = true;
    if (g_keptSaid == static_cast<int>(until)) return;
    g_keptSaid = static_cast<int>(until);
    UE_LOGI("weather: the host's Bad Sun %d kept until %s", payload.state != 0 ? 1 : 0,
            until == Until::World ? "this world has a gamemode and a day cycle" : "the object index can drain");
}

}  // namespace

void SetSession(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Install() {
    if (g_gateAsked) return;
    g_gateAsked = true;
    if (!sg::WatchName(kRowSpawnBadSun, kSpawnTag, &OnSpawnPre, &OnSpawnPost)) {
        g_spawnWatch.said = g_spawnWatch.settled = true;  // the refusal is said here; there is no watch to wait on
        UE_LOGE("weather: the script gate refused the watch on mainGamemode_C.Spawn Bad Sun -- the host's fresh Bad "
                "Sun does not cross, and a client's own Spawn Bad Sun runs");
    }
    if (!sg::WatchName(kRowRemove, kRemoveTag, nullptr, &OnRemovePost)) {
        g_removeWatch.said = g_removeWatch.settled = true;
        UE_LOGE("weather: the script gate refused the watch on badSun_C.remove -- client copies still end by their "
                "clocks; the host's removal sends nothing");
    }
}

void Tick() {
    // The host's payload that waited for a gamemode and a day cycle, or for the index to drain. Apply clears it as it
    // proceeds and keeps it again should the world have lost either since.
    if (g_hasPending && GT::IsGameThread() && IsClientSession() && ReadCycle() == Cycle::Present) {
        const coop::net::BadSunPayload p = g_pending;
        if (g_keptSaid != static_cast<int>(Until::Index))  // an index retry says nothing each tick
            UE_LOGI("weather: applying the Bad Sun %d kept for this world", p.state != 0 ? 1 : 0);
        Apply(p);
    }
    if (g_spawnWatch.said && g_removeWatch.said) return;
    if (!g_gateAsked) return;
    // The class loads lazily: the gate asks it when the class set moves, so the names resolve while a watch is
    // unsettled and the class judgement runs when the set moved.
    const uint64_t version = ue_wrap::object_index::ClassSetVersion();
    const bool classMoved = !g_seenVersionValid || version != g_seenVersion;
    // The gate judges a watch's class inside this resolve, so a moved class set runs it for a settled watch too.
    if (!g_spawnWatch.settled || !g_removeWatch.settled || classMoved) sg::ResolvePendingNames();
    SayWatch(g_spawnWatch, classMoved);
    SayWatch(g_removeWatch, classMoved);
    g_seenVersionValid = true;
    g_seenVersion = version;
}

bool ReadLocalBadSun(bool& present) {
    present = false;
    return GT::IsGameThread() && BS::ReadGamemodeCopy(nullptr, present);
}

void Apply(const coop::net::BadSunPayload& payload) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: the host's Bad Sun applied off the game thread -- dropping");
        return;
    }
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h). A task posted
    // before a session's stop and run after it finds no session, and a host holds nothing: neither keeps a payload.
    if (!IsClientSession()) return;
    const bool want = payload.state != 0;
    // No gamemode, or no day cycle in it (a world a copy cannot run in): the payload waits, the newest of them,
    // and Tick applies it once this world has both.
    if (ReadCycle() != Cycle::Present) {
        Keep(payload, Until::World);
        return;
    }
    // A payload that proceeds is newer than any kept one: the kept slot never outlives it.
    g_hasPending = false;
    void* const world = ue_wrap::world_identity::CurrentWorld();
    const std::wstring worldName = world ? R::ToString(R::NameOf(world)) : std::wstring(L"?");
    const size_t backlog = ue_wrap::object_index::Backlog();
    void* copies[BS::kMaxLive] = {};
    int n = BS::CountLive(copies, BS::kMaxLive);
    if (n < 0) {
        Keep(payload, Until::Index);  // the next Tick asks again
        return;
    }
    g_keptSaid = -1;
    g_lastApplied = true;
    g_lastWant = want;
    g_lastLive = n;
    g_lastSpawned = false;
    UE_LOGI("weather: apply Bad Sun %d in world %ls -- index backlog %zu, %d live cop%s", want ? 1 : 0,
            worldName.c_str(), backlog, n, n == 1 ? "y" : "ies");
    if (want) {
        if (n == 0) {
            bool ran = false;
            {
                Applying scope;
                ran = BS::CallSpawn();
            }
            n = BS::CountLive(copies, BS::kMaxLive);
            if (ran && n == 1) {
                ++g_spawned;
                g_lastSpawned = true;
                UE_LOGI("weather: applied the host's Bad Sun 1 -- spawned one copy (%s)", StateOf(copies[0]));
            } else {
                char why[96];
                std::snprintf(why, sizeof(why), "Spawn Bad Sun ran=%d, then %d live cop%s", ran ? 1 : 0, n,
                              n == 1 ? "y" : "ies");
                FailedApply(why, want);
            }
        } else if (n >= 2) {
            for (int i = 0; i < n && i < BS::kMaxLive; ++i)
                UE_LOGE("weather: %d Bad Sun copies live on this peer, the host has one -- copy %d at %p is %s; "
                        "nothing repaired", n, i, copies[i], StateOf(copies[i]));
        }
        return;
    }
    for (int i = 0; i < n && i < BS::kMaxLive; ++i) {
        bool active = false;
        if (!BS::ReadActive(copies[i], active)) {
            FailedApply("a copy's widget did not read", want);
        } else if (active) {
            // MTA's client applies a host's "none" at once
            // (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:2262-2265). Leaves it here: a
            // forced remove() would cut the copy's fade and skip the widget's sky reset, while the copy's own fade
            // is due within the clock's sync lag.
            ++g_leftActive;
            UE_LOGI("weather: the host's Bad Sun 0 -- copy %d at %p is active, left to end by its own clock", i,
                    copies[i]);
        } else if (BS::DestroyInactive(copies[i])) {
            ++g_destroyed;
            UE_LOGI("weather: the host's Bad Sun 0 -- destroyed the inactive copy %d at %p", i, copies[i]);
        } else {
            FailedApply("an inactive copy was not destroyed", want);
        }
    }
}

Counts GetCounts() {
    Counts c;
    c.refused = g_refused;
    c.sent = g_sent;
    c.sentAbsent = g_sentAbsent;
    c.spawned = g_spawned;
    c.destroyed = g_destroyed;
    c.leftActive = g_leftActive;
    c.applyFailed = g_applyFailed;
    c.removesRan = g_removesRan;
    c.lastApplied = g_lastApplied;
    c.lastApplyWant = g_lastWant;
    c.lastApplySpawned = g_lastSpawned;
    c.lastApplyLive = g_lastLive;
    return c;
}

void OnDisconnect() {
    if (g_refused || g_sent || g_spawned || g_destroyed || g_leftActive || g_applyFailed)
        UE_LOGI("weather: Bad Sun session end -- %llu of this client's own spawns refused, %llu sent, %llu spawned, "
                "%llu inactive copies destroyed, %llu active copies left to end, %llu did not apply",
                static_cast<unsigned long long>(g_refused), static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_spawned), static_cast<unsigned long long>(g_destroyed),
                static_cast<unsigned long long>(g_leftActive), static_cast<unsigned long long>(g_applyFailed));
    g_refused = g_sent = g_spawned = g_destroyed = g_leftActive = g_applyFailed = 0;
    g_sentAbsent = g_removesRan = 0;
    g_lastApplied = g_lastWant = g_lastSpawned = false;
    g_lastLive = 0;
    g_saidRefused = false;
    g_hasPending = false;
    g_pending = {};
    g_keptSaid = -1;
    g_saidCycleUnreadable = false;
    g_applying = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::weather_badsun
