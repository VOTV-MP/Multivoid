// coop/interactables/cremator_state.cpp -- see coop/interactables/cremator_state.h.

#include "coop/interactables/cremator_state.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/interactable_sync.h"  // the lane's key, and the host's edge
#include "coop/net/session.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace coop::cremator_state {
namespace {

namespace C  = ue_wrap::cremator;
namespace sg = ue_wrap::script_gate;

// The handle's finish is the lever run's edge: the timeline's last tick sets or clears `isActive`, so the body is
// watched after it has run. useLever is the press's and funnyClose's entry, refused on a client's copy. A lid's
// close, after its body, is the moment a copy that waited on its open door may start the run.
UE_FACT_FN(kRowFinished, "cremator_state", L"cremator_C", L"handleLimeline__FinishedFunc", Declared);
UE_FACT_FN(kRowUseLever, "cremator_state", L"cremator_C", L"useLever", Declared);
UE_FACT_FN(kRowDoorClose, "cremator_state", L"prop_swinger_C", L"close", Declared);

constexpr int kTagFinished  = 0x43524653;  // 'CRFS'
constexpr int kTagUseLever  = 0x4352554C;  // 'CRUL'
constexpr int kTagDoorClose = 0x43524443;  // 'CRDC'

struct Watch { const ue_wrap::facts::Function* row; int tag; sg::PreFn pre; sg::PostFn post; };

std::atomic<coop::net::Session*> g_session{nullptr};

// The state the host last told this client for a cremator. A held want outlives the drive: the copy's flag is
// compared with it at each look, and it is replaced by the next state, or dropped with the session or the world.
struct Want {
    bool on = false;
    bool doorSaid = false;  // the wait on an open door is said once a streak
    ue_wrap::CachedObjRef ref;
};
std::unordered_map<void*, Want> g_wants;
// The doors of the cremators that hold a want, each to its cremator: only a close of one of these re-evaluates a
// want, and any other lid's close costs one lookup. Pointers are compared, never dereferenced.
std::unordered_map<void*, void*> g_doors;
uint32_t g_wantsGen = 0;

constexpr int kMaxHostRunListeners = 4;
std::array<HostRunFn, kMaxHostRunListeners> g_hostRun{};
int g_hostRunCount = 0;

uint64_t g_drives = 0, g_leverRefused = 0, g_hostEdges = 0, g_doorWaits = 0;
bool g_unreadSaid = false;

// The dev red arm of the lever drill (cremator_state_off): the host reports no state, so the channel sends no edge and
// no connect snapshot, and a client's copy stays idle.
bool StateOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::cremator_state_off);
    return off;
}

// The wants belong to one world: a world change empties them, the index being stamped the same way.
void SyncGeneration() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_wantsGen) return;
    g_wantsGen = gen;
    g_wants.clear();
    g_doors.clear();
}

Want* HeldWant(void* cremator) {
    SyncGeneration();
    const auto it = g_wants.find(cremator);
    return (it != g_wants.end() && it->second.ref.Get() == cremator) ? &it->second : nullptr;
}

// Brings the copy's handle to the held want, when the handle is at rest and the flag differs. A handle mid-travel
// is left alone: its own finish looks again. A run is started only with the copy's door shut, as the machine refuses
// to start a burn with the door open: a forward finish at an open door hints and rewinds the handle with `anim`
// already false, so that finish looks again at once. The drive never closes the door itself: the host shuts its
// lid before it starts the run and the lid lane brings that to the copy, so an open door is waited on, said once a
// streak, and the door's own close (OnDoorClosePost) looks again. A reverse needs no shut door.
void Drive(void* cremator) {
    Want* w = HeldWant(cremator);
    if (!w) return;
    bool anim = false, active = false;
    if (!C::ReadAnim(cremator, anim) || !C::ReadActive(cremator, active)) {
        if (!g_unreadSaid) {
            g_unreadSaid = true;
            UE_LOGW("[CREMATOR-STATE] the copy's anim or isActive did not read -- its handle is not driven (said once)");
        }
        return;
    }
    if (anim || active == w->on) return;
    bool doorOpen = false;
    if (w->on && C::ReadDoorOpened(cremator, doorOpen) && doorOpen) {
        ++g_doorWaits;
        if (!w->doorSaid) {
            w->doorSaid = true;
            UE_LOGI("[CREMATOR-STATE] key='%ls': this copy's door reads open -- its handle waits for the door to shut",
                    coop::interactable_sync::CrematorKey(cremator).c_str());
        }
        return;
    }
    w->doorSaid = false;
    const bool ran = C::CallUseLever(cremator);
    ++g_drives;
    UE_LOGI("[CREMATOR-STATE] key='%ls': this copy's handle driven to %s (isActive %d) ran=%d",
            coop::interactable_sync::CrematorKey(cremator).c_str(), w->on ? "ON" : "OFF", active ? 1 : 0, ran ? 1 : 0);
}

// After the body: the handle is at rest. The host sends the flag it left; a client looks again at its want.
//
// Divergence from MTA, deliberate: MTA broadcasts the start of an act (CGame.cpp:3196, VEHICLE_REQUEST_IN_CONFIRMED)
// and confirms its end (CGame.cpp:3373), so every copy animates at once.
// Here the host sends only the COMMITTED `isActive` at the handle's finish: a start sent at the press could show a
// client a run that the host's own door check (the forward finish at an open door) still rewinds. The price is a
// client display that trails the host's by about one handle travel plus the trip (docs/devices.md). The host's lid
// is locked only at that finish, so from the press until then a client's own open of the lid reaches the host's
// unlocked lid and the host turns the start back at the finish with the open-door hint, as for its own player; the
// client sees nothing.
void OnFinishedPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object || !C::IsCremator(call.object)) return;
    if (s->role() == coop::net::Role::Host) {
        ++g_hostEdges;
        coop::interactable_sync::OnCrematorVerb(call.object);
        bool active = false;
        if (g_hostRunCount > 0 && C::ReadActive(call.object, active))
            for (int i = 0; i < g_hostRunCount; ++i) g_hostRun[i](call.object, active);
        return;
    }
    Drive(call.object);
}

// After any lid's close body, on a client: when the lid is a held want's door, the want is looked at again, since
// the copy may have waited on that door. Every other lid costs one lookup.
void OnDoorClosePost(const sg::Call& call) {
    if (!call.object) return;
    const auto it = g_doors.find(call.object);
    if (it == g_doors.end()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return;
    // Drive reads the want through HeldWant, which drops a stale world's wants and doors first.
    Drive(it->second);
}

// Before the body, on a client: its copy's handle moves only by the lane's own drive (a call inside our own
// dispatch). The press is refused where it is made (coop/interactables/cremator_intent); what is left is funnyClose's
// call, the machine's own start of a run, which is the host's.
sg::Verdict OnUseLeverPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (call.fromOurCode || !call.object || !C::IsCremator(call.object)) return sg::Verdict::Run;
    if (++g_leverRefused <= 3 || g_leverRefused % 20 == 0)
        UE_LOGI("[CREMATOR-STATE] a client's own useLever on key='%ls' is refused -- the host's copy runs the lever "
                "(#%llu)", coop::interactable_sync::CrematorKey(call.object).c_str(),
                static_cast<unsigned long long>(g_leverRefused));
    return sg::Verdict::Cancel;
}

const Watch kWatches[] = {
    { &kRowFinished, kTagFinished, nullptr, &OnFinishedPost },
    { &kRowUseLever, kTagUseLever, &OnUseLeverPre, nullptr },
    { &kRowDoorClose, kTagDoorClose, nullptr, &OnDoorClosePost },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles once every
// watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, kWatches[i].pre, kWatches[i].post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[CREMATOR-STATE] the script-body gate refused the watch on '%ls' -- for the rest of this process the "
                "cremator's lever run cannot be seen or held there", kWatches[i].row->Name());
    }
}

}  // namespace

bool ReadState(void* cremator, bool& on) {
    if (StateOff()) return false;
    if (const Want* w = HeldWant(cremator)) {
        on = w->on;
        return true;
    }
    return C::ReadActive(cremator, on);
}

bool ApplyState(void* cremator, bool on) {
    if (!C::IsCremator(cremator)) return false;
    SyncGeneration();
    for (auto it = g_wants.begin(); it != g_wants.end();)
        it = it->second.ref.Alive() ? std::next(it) : g_wants.erase(it);
    for (auto it = g_doors.begin(); it != g_doors.end();)
        it = g_wants.count(it->second) ? std::next(it) : g_doors.erase(it);
    Want& w = g_wants[cremator];
    if (w.on != on) w.doorSaid = false;   // a new streak says its wait once
    w.on = on;
    w.ref.Set(cremator);
    if (void* door = C::Door(cremator)) g_doors[door] = cremator;
    Drive(cremator);
    return true;
}

bool AddHostRunListener(HostRunFn fn) {
    if (!fn) return false;
    for (int i = 0; i < g_hostRunCount; ++i)
        if (g_hostRun[i] == fn) return true;
    if (g_hostRunCount == kMaxHostRunListeners) return false;
    g_hostRun[g_hostRunCount++] = fn;
    return true;
}

bool HeldWantOfDoor(void* door, void*& cremator, bool& wantOn) {
    SyncGeneration();
    const auto it = g_doors.find(door);
    if (it == g_doors.end()) return false;
    const Want* w = HeldWant(it->second);
    if (!w) return false;
    cremator = it->second;
    wantOn = w->on;
    return true;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[CREMATOR-STATE] the cremator gates are live: the handle's finish, useLever and a lid's close");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[CREMATOR-STATE] %d of %d cremator gates are dead (refused, or resolved into a full table) -- the "
                "lever run cannot be seen or held there", kWatchCount - live, kWatchCount);
    }
}

void OnDisconnect() {
    if (g_drives || g_leverRefused || g_hostEdges || g_doorWaits)
        UE_LOGI("[CREMATOR-STATE] session end -- the host saw %llu handle finish(es); a client drove its copy %llu "
                "time(s), refused %llu own useLever call(s), held its drive for an open door %llu time(s)",
                static_cast<unsigned long long>(g_hostEdges), static_cast<unsigned long long>(g_drives),
                static_cast<unsigned long long>(g_leverRefused), static_cast<unsigned long long>(g_doorWaits));
    g_drives = g_leverRefused = g_hostEdges = g_doorWaits = 0;
    g_unreadSaid = false;
    g_wants.clear();
    g_doors.clear();
}

}  // namespace coop::cremator_state
