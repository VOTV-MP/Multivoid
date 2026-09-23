// coop/world/event_active_sync.cpp -- see coop/world/event_active_sync.h.
//
// Bytecode ground truth, from the blueprint:
//   - lib_C::setEvent(isEventActive, deactivateAmbientTrack, __WorldContext) -- the context
//     is the LAST parameter. Active adds 1 to gamemode.activeEvents and appends the context
//     to activeEvents_senders; inactive subtracts 1, removes the context, and clamps a
//     negative count back to 0. The sender registers ITSELF, so ClassOf(sender) names the
//     event class.
//   - lib_C::getEvent is `activeEvents > 0` OR the player camera being outside the base box.
//     The native no-save-during-event gate reads it; single player does not even pause
//     mid-event.
//   - Around ninety-five classes call setEvent: creature controllers, story events, pranks,
//     ambience. Several can be active at once, which is why the game keeps a refcount rather
//     than a bool.

#include "coop/world/event_active_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/event_fire_sync.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/active_events.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::event_active_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace AE = ue_wrap::active_events;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- host poll state (game thread) -----------------------------------------------------------
struct ActiveEntry {
    int32_t objIdx;         // sender's GUObjectArray internal index (recycled-slot-safe liveness)
    std::string className;  // ClassOf(sender) name, narrowed (the event's implementation class)
    long long firstSeenMs;  // steady-clock ms when the poll first saw it (elapsedSec source)
};
std::unordered_map<void*, ActiveEntry> g_active;  // sender ptr -> entry
void* g_polledGm = nullptr;                       // the instance the membership belongs to
int32_t g_polledGmIdx = -1;
bool g_primed = false;
long long g_lastPollMs = 0;
constexpr long long kPollIntervalMs = 1000;  // event phases run seconds-to-minutes; 1 Hz is generous

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- the class->row map ------------------------------------------------------------------
// ClassOf(sender) is the event's IMPLEMENTATION class, not the list_events row the replay
// policy is keyed by. A WRONG entry replays the wrong event, and a MISSING one logs LOUD on
// the receiver and names the class to add, so a row is added only once its link has been read
// out of the runEvent case table or seen live -- never guessed. Classes that register but map to no scheduled row (sub-chains like
// trigger_alarm_C, weather senders, creature controllers spawned BY events) stay unmapped by
// design: their state rides their own lanes, not replay.
struct ClassRow { const char* className; const char* rowName; };
const ClassRow kClassRowMap[] = {
    { "obelisk_C", "obelisk" },                         // proven live
    { "piramid2_C", "piramid" },                        // proven live
    { "trigger_solarBoom_C", "solar" },
    { "trigger_vehtp_C", "vehtp" },
    { "trigger_agrav_C", "agrav" },
    { "trigger_bigmRoar_C", "call0" },
    { "trigger_wispSwarm_C", "wisps" },
    { "trigger_spawnFollowingArir_C", "arirFollower" },
    { "trigger_arirEgg_C", "arirEgg" },
    { "trigger_bedEvent_C", "bedEvent" },
    { "tentacleBallsFollower_C", "tentacleBalls" },
    { "soltomiaCleaning_C", "soltoClean" },
    { "morningUfo_C", "morningGay" },
    { "rozitBorg_C", "borgRozital" },
    { "event_bottomHoleController_C", "rozitalHole" },
    { "ventCrawler_C", "ventCrawler" },
    { "kocker_C", "ventKnocker" },
    { "grayEventController_C", "graysforest" },
    { "arirBusterSpawner_C", "arirBuster" },
    { "saltpile_C", "salt" },
    { "superEgger_C", "eggvasion" },
    { "boarInvasion_C", "boarwar" },
    { "dreamer_dreambase_C", "dreambase" },
    { "arirShip_C", "arirShip" },
};

const char* RowForClass(const std::string& className) {
    for (const auto& e : kClassRowMap)
        if (className == e.className) return e.rowName;
    return nullptr;
}

// Registrants whose state a dedicated LANE snapshots at the same join edge. An EventSnapshot
// for them would only ship an unmapped-row WARN to every joiner, so they are skipped with an
// INFO and the WARN stays meaningful for genuinely uncovered classes.
struct LaneOwned { const char* className; const char* lane; };
const LaneOwned kLaneOwnedClasses[] = {
    { "trigger_alarm_C", "alarm_sync" },
};
const char* LaneFor(const std::string& className) {
    for (const auto& e : kLaneOwnedClasses)
        if (className == e.className) return e.lane;
    return nullptr;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

int ReadRefcount() {
    int32_t n = 0;
    AE::ReadCount(n);
    return n;
}

void HostPollTick() {
    if (!AE::EnsureResolved()) return;  // before Gamemode(): the latched-OFF failure mode must not keep a cache warm nothing reads
    int32_t gmIdx = -1;
    void* gm = AE::Gamemode(&gmIdx);
    if (!gm) return;
    // World/save reload minted a new gamemode -> the old membership's pointers dangle. Drop and
    // re-prime against the new instance (already-active events log BEGIN fresh -- correct: they
    // ARE in flight in the new world).
    if (gm != g_polledGm || !R::IsLiveByIndex(g_polledGm, g_polledGmIdx)) {
        g_active.clear();
        g_polledGm = gm;
        g_polledGmIdx = gmIdx;
        g_primed = false;
    }
    AE::Senders arr{};
    if (!AE::ReadSenders(arr)) return;
    if (arr.num < 0 || arr.num > 4096) return;  // sanity: ~95 registrant classes, few concurrent
    const long long now = NowMs();
    if (!g_primed) {
        g_primed = true;
        UE_LOGI("event_active: host poll primed (n=%d active)", ReadRefcount());
    }
    // BEGIN edges: senders in the array we aren't tracking yet.
    for (int32_t i = 0; i < arr.num; ++i) {
        void* obj = arr.data ? arr.data[i] : nullptr;
        if (!obj || g_active.count(obj)) continue;
        if (!R::IsLive(obj)) continue;  // freshly read from the engine array; defensive
        ActiveEntry e;
        e.objIdx = R::InternalIndexOf(obj);
        e.className = Narrow(R::ClassNameOf(obj));
        e.firstSeenMs = now;
        UE_LOGI("event_active: BEGIN class=%s n=%d (senders=%d)",
                e.className.c_str(), ReadRefcount(), arr.num);
        g_active.emplace(obj, std::move(e));
    }
    // END edges: tracked senders gone from the array (deregistered), or dead without
    // deregistering (destroyed actor; the game's own clamp guards the same case).
    std::vector<void*> ended;
    for (auto& [obj, e] : g_active) {
        bool present = false;
        if (arr.data)
            for (int32_t i = 0; i < arr.num; ++i)
                if (arr.data[i] == obj) { present = true; break; }
        const bool live = R::IsLiveByIndex(obj, e.objIdx);
        if (present && live) continue;
        UE_LOGI("event_active: END class=%s n=%d elapsed=%llds%s",
                e.className.c_str(), ReadRefcount(), (now - e.firstSeenMs) / 1000,
                live ? "" : " (sender died unregistered)");
        ended.push_back(obj);
    }
    for (void* obj : ended) g_active.erase(obj);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;
    HostPollTick();
}

void SendJoinSnapshotForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (g_active.empty()) {
        UE_LOGI("event_active: join-edge slot=%d -- 0 in flight (no EventSnapshot needed)", slot);
        return;
    }
    const long long now = NowMs();
    for (const auto& [obj, e] : g_active) {
        if (const char* lane = LaneFor(e.className)) {
            UE_LOGI("event_active: join-edge slot=%d class=%s is LANE-OWNED (%s snapshots it) "
                    "-- no EventSnapshot",
                    slot, e.className.c_str(), lane);
            continue;
        }
        const char* row = RowForClass(e.className);
        const long long elapsed = (now - e.firstSeenMs) / 1000;
        coop::net::EventSnapshotPayload p{};  // zero-init -> both name[]s pre-NUL-bound
        std::strncpy(p.className, e.className.c_str(), sizeof(p.className) - 1);
        if (row) std::strncpy(p.rowName, row, sizeof(p.rowName) - 1);
        p.elapsedSec = static_cast<uint16_t>(elapsed < 0 ? 0 : (elapsed > 65535 ? 65535 : elapsed));
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::EventSnapshot, &p, sizeof(p))) {
            UE_LOGI("event_active: join-edge slot=%d SNAPSHOT class=%s row=%s elapsed=%llds",
                    slot, e.className.c_str(), row ? row : "<unmapped>", elapsed);
        } else {
            UE_LOGW("event_active: join-edge slot=%d EventSnapshot send FAILED (class=%s)",
                    slot, e.className.c_str());
        }
    }
}

void OnReliable(const coop::net::EventSnapshotPayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_active: OnReliable off-game-thread -- dropping"); return; }
    // NUL-bound both names (payload crosses the trust boundary; the dispatcher length-checked it).
    char cls[sizeof(payload.className) + 1] = {};
    std::memcpy(cls, payload.className, sizeof(payload.className));
    char row[sizeof(payload.rowName) + 1] = {};
    std::memcpy(row, payload.rowName, sizeof(payload.rowName));
    if (cls[0] == '\0') {
        UE_LOGW("event_active: EventSnapshot with empty className -- dropping");
        return;
    }
    if (row[0] == '\0') {
        // The fill signal: this exact line names the class the map is missing.
        UE_LOGW("event_active: in-flight event class=%s elapsed=%us has NO class->row map entry "
                "-- skipped (add it to kClassRowMap; lanes still deliver lane-owned state)",
                cls, static_cast<unsigned>(payload.elapsedSec));
        return;
    }
    UE_LOGI("event_active: join snapshot -- in-flight class=%s row=%s elapsed=%us",
            cls, row, static_cast<unsigned>(payload.elapsedSec));
    coop::event_fire_sync::ReplayInFlightRow(row);
}

void OnDisconnect() {
    g_active.clear();
    g_polledGm = nullptr;
    g_polledGmIdx = -1;
    g_primed = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::event_active_sync
