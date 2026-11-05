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

#include <atomic>
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace coop::cremator_state {
namespace {

namespace C  = ue_wrap::cremator;
namespace sg = ue_wrap::script_gate;

// The handle's finish is the lever run's edge: the timeline's last tick sets or clears `isActive`, so the body is
// watched after it has run. useLever is the press's and funnyClose's entry, refused on a client's copy.
UE_FACT_FN(kRowFinished, "cremator_state", L"cremator_C", L"handleLimeline__FinishedFunc", Declared);
UE_FACT_FN(kRowUseLever, "cremator_state", L"cremator_C", L"useLever", Declared);

constexpr int kTagFinished = 0x43524653;  // 'CRFS'
constexpr int kTagUseLever = 0x4352554C;  // 'CRUL'

struct Watch { const ue_wrap::facts::Function* row; int tag; sg::PreFn pre; sg::PostFn post; };

std::atomic<coop::net::Session*> g_session{nullptr};

// The state the host last told this client for a cremator. A held want outlives the drive: the copy's flag is
// compared with it at each look, and it is replaced by the next state, or dropped with the session or the world.
struct Want {
    bool on = false;
    ue_wrap::CachedObjRef ref;
};
std::unordered_map<void*, Want> g_wants;
uint32_t g_wantsGen = 0;

uint64_t g_drives = 0, g_leverRefused = 0, g_hostEdges = 0, g_doorNotShut = 0;
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
}

const Want* HeldWant(void* cremator) {
    SyncGeneration();
    const auto it = g_wants.find(cremator);
    return (it != g_wants.end() && it->second.ref.Get() == cremator) ? &it->second : nullptr;
}

// Brings the copy's handle to the held want, when the handle is at rest and the flag differs. A handle mid-travel
// is left alone: its own finish looks again, and so a forward finish that the open door turned back (the machine
// hints and rewinds the handle) reaches the want by the rewind's finish. The door is shut first when the run starts,
// as the machine refuses to start a burn with the door open.
void Drive(void* cremator) {
    const Want* w = HeldWant(cremator);
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
    bool wasOpened = false;
    if (w->on && !C::ShutDoor(cremator, wasOpened)) {
        ++g_doorNotShut;
        UE_LOGW("[CREMATOR-STATE] key='%ls': the open door's Close() did not run before the run",
                coop::interactable_sync::CrematorKey(cremator).c_str());
    }
    const bool ran = C::CallUseLever(cremator);
    ++g_drives;
    UE_LOGI("[CREMATOR-STATE] key='%ls': this copy's handle driven to %s (isActive %d, door was %s) ran=%d",
            coop::interactable_sync::CrematorKey(cremator).c_str(), w->on ? "ON" : "OFF", active ? 1 : 0,
            wasOpened ? "open" : "shut", ran ? 1 : 0);
}

// After the body: the handle is at rest. The host sends the flag it left; a client looks again at its want.
void OnFinishedPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object || !C::IsCremator(call.object)) return;
    if (s->role() == coop::net::Role::Host) {
        ++g_hostEdges;
        coop::interactable_sync::OnCrematorVerb(call.object);
        return;
    }
    Drive(call.object);
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
    Want& w = g_wants[cremator];
    w.on = on;
    w.ref.Set(cremator);
    Drive(cremator);
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
        UE_LOGI("[CREMATOR-STATE] the cremator gates are live: the handle's finish and useLever");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[CREMATOR-STATE] %d of %d cremator gates are dead (refused, or resolved into a full table) -- the "
                "lever run cannot be seen or held there", kWatchCount - live, kWatchCount);
    }
}

void OnDisconnect() {
    if (g_drives || g_leverRefused || g_hostEdges || g_doorNotShut)
        UE_LOGI("[CREMATOR-STATE] session end -- the host saw %llu handle finish(es); a client drove its copy %llu "
                "time(s), refused %llu own useLever call(s), left the door open %llu time(s)",
                static_cast<unsigned long long>(g_hostEdges), static_cast<unsigned long long>(g_drives),
                static_cast<unsigned long long>(g_leverRefused), static_cast<unsigned long long>(g_doorNotShut));
    g_drives = g_leverRefused = g_hostEdges = g_doorNotShut = 0;
    g_unreadSaid = false;
    g_wants.clear();
}

}  // namespace coop::cremator_state
