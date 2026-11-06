// coop/interactables/cremator_client_guard.cpp -- see coop/interactables/cremator_client_guard.h.

#include "coop/interactables/cremator_client_guard.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/cremator_state.h"      // the cremator a door belongs to, and the want held for it
#include "coop/interactables/interactable_sync.h"   // the lane's key, for the lines
#include "coop/net/session.h"
#include "coop/session/net_pump.h"                  // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_set>

namespace coop::cremator_client_guard {
namespace {

namespace C  = ue_wrap::cremator;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// addObject is the cremator's own collection of one actor into `actors`; the lid's open is prop_swinger_C's, so the
// door guard looks the lid up in the lane's door set first and only then at the machine; actionOptionIndex is the
// press, whose handle half is cremator_intent's.
UE_FACT_FN(kRowAddObject, "cremator_client_guard", L"cremator_C", L"addObject", Declared);
UE_FACT_FN(kRowDoorOpen, "cremator_client_guard", L"prop_swinger_C", L"open", Declared);
UE_FACT_FN(kRowValve, "cremator_client_guard", L"cremator_C", L"actionOptionIndex", Declared);
// The blow-out's 0.2 s timer: it spawns a prop_crematorFireball_C while the machine is active and its lid open.
UE_FACT_FN(kRowSpit, "cremator_client_guard", L"cremator_C", L"spitFireballs_timer", Declared);

constexpr int kTagAddObject = 0x43474144;  // 'CGAD'
constexpr int kTagDoorOpen  = 0x4347444F;  // 'CGDO'
constexpr int kTagValve     = 0x43475650;  // 'CGVP'
constexpr int kTagSpit      = 0x43475346;  // 'CGSF'

// A refusal hint through the game's own lib_C::addHint, type 1 as the cremator's "The door is open" and the gas can's
// "Gas tanks is empty". The game has no hint for a refused valve (its valve press toggles `pouring` and says nothing),
// so the text is ours: what the player did, and who pours.
constexpr uint8_t kHintRefusal = 1;
constexpr const wchar_t* kValveHint = L"Turning the valve does nothing: the host pours the fuel";

// A valve press is one line a burst: the next press within this gap belongs to the same burst.
constexpr uint64_t kBurstGapMs = 3000;

std::atomic<coop::net::Session*> g_session{nullptr};

uint64_t g_collectRefused = 0, g_doorRefused = 0, g_valveRefused = 0, g_valveBursts = 0, g_doorLoadNative = 0;
uint64_t g_spitRefused = 0;
bool g_spitSaid = false;
uint64_t g_valveLastMs = 0;
bool g_valveEver = false;
bool g_actionSaid = false;

// The copies whose first refusal was said: pointers compared, never dereferenced, one set per world.
std::unordered_set<void*> g_collectSaid;
uint32_t g_saidGen = 0;

R::ParamOffset g_action{L"action"};

// The dev red arm (cremator_client_guard_off): all four gates run the Blueprint's own body, so a client's copy
// collects the sitter, opens its door, turns its valve and spits fireballs on its own account.
bool GuardOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::cremator_client_guard_off);
    return off;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// True on a client whose session runs; the other cremator watches read the role the same way.
bool OnClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

void SyncGeneration() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_saidGen) return;
    g_saidGen = gen;
    g_collectSaid.clear();
}

// Before the body, on a client: the copy collects nothing. The Blueprint adds the actor to `actors`, `health` and
// `giveFuel` there and nowhere else a dumped graph reaches, so the copy's tick has no one to burn and no prop to
// destroy; the burn a client's player takes comes from the host's machine (cremator_hit). A call before the client's
// world is announced is refused too: the copy's own begin-overlap during its load collects nobody either.
sg::Verdict OnAddObjectPre(const sg::Call& call) {
    if (GuardOff() || !OnClient()) return sg::Verdict::Run;
    if (!call.object || !C::IsCremator(call.object)) return sg::Verdict::Run;
    ++g_collectRefused;
    SyncGeneration();
    if (g_collectSaid.insert(call.object).second)
        UE_LOGI("[CREMATOR-GUARD] a client's cremator copy key='%ls' collects nothing: its addObject is refused (first "
                "for this copy; the rest are counted)", coop::interactable_sync::CrematorKey(call.object).c_str());
    return sg::Verdict::Cancel;
}

// Before the body, on a client: a cremator's door is not opened by a player's use while its copy runs. The host's
// door is locked for the whole run (the machine's setClosed), so its own open() refuses; a client's copy is unlocked
// until the host's state reaches it. The lane's own apply and a call of the client's own world load pass.
sg::Verdict OnDoorOpenPre(const sg::Call& call) {
    if (GuardOff() || !OnClient() || !call.object) return sg::Verdict::Run;
    void* cremator = nullptr;
    bool wantOn = false;
    if (!coop::cremator_state::HeldWantOfDoor(call.object, cremator, wantOn)) return sg::Verdict::Run;  // another lid
    if (call.fromOurCode) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) {
        ++g_doorLoadNative;
        return sg::Verdict::Run;
    }
    bool active = false;
    if (!wantOn && !(C::ReadActive(cremator, active) && active)) return sg::Verdict::Run;
    if (++g_doorRefused <= 3 || g_doorRefused % 20 == 0)
        UE_LOGI("[CREMATOR-GUARD] a client's own open of the door of cremator key='%ls' is refused while its run is "
                "on (#%llu)", coop::interactable_sync::CrematorKey(cremator).c_str(),
                static_cast<unsigned long long>(g_doorRefused));
    return sg::Verdict::Cancel;
}

// Before the body, on a client: a press of the fuel valve is refused and not sent. The pour runs from `gasAmount`,
// which does not cross, so the copy holds no fuel to show; the host pours until the gasoline package carries it.
sg::Verdict OnValvePre(const sg::Call& call) {
    if (GuardOff() || !OnClient()) return sg::Verdict::Run;
    if (!call.object || !C::IsCremator(call.object)) return sg::Verdict::Run;
    const int32_t off = g_action.Of(call.function);
    if (off < 0) {
        if (!g_actionSaid) {
            g_actionSaid = true;
            UE_LOGW("[CREMATOR-GUARD] the press's action parameter did not resolve -- a valve press runs here alone "
                    "(said once)");
        }
        return sg::Verdict::Run;
    }
    if (*(call.locals + off) != C::kPressAction) return sg::Verdict::Run;
    bool valve = false, handle = false;
    if (!C::ReadLookingAtValve(call.object, valve) || !valve) return sg::Verdict::Run;
    if (C::ReadLookingAtHandle(call.object, handle) && handle) return sg::Verdict::Run;  // the handle's press
    ++g_valveRefused;
    const uint64_t now = NowMs();
    if (!g_valveEver || now - g_valveLastMs > kBurstGapMs) {
        ++g_valveBursts;
        UE_LOGI("[CREMATOR-GUARD] a client cannot open the cremator's fuel valve until the gasoline package carries the "
                "fuel; the host pours (key='%ls', burst #%llu)", coop::interactable_sync::CrematorKey(call.object).c_str(),
                static_cast<unsigned long long>(g_valveBursts));
        // The first press of a burst only: the player is told through the game's own hint, as the machine does for its
        // own refusals.
        ue_wrap::votv_lib::AddHint(kValveHint, kHintRefusal, call.object);
    }
    g_valveEver = true;
    g_valveLastMs = now;
    return sg::Verdict::Cancel;
}

// Before the body, on a client: the copy spits no fireballs. The host's blow-out opens the copy's lid through the lid
// lane while the copy is active, and its own 0.2 s timer would then spawn a local fireball each tick, unsynced,
// beside the host's. Counted, said once a session.
sg::Verdict OnSpitPre(const sg::Call& call) {
    if (GuardOff() || !OnClient()) return sg::Verdict::Run;
    if (!call.object || !C::IsCremator(call.object)) return sg::Verdict::Run;
    ++g_spitRefused;
    if (!g_spitSaid) {
        g_spitSaid = true;
        UE_LOGI("[CREMATOR-GUARD] a client's cremator copy key='%ls' spits no fireballs: its spitFireballs_timer is "
                "refused (the rest are counted)", coop::interactable_sync::CrematorKey(call.object).c_str());
    }
    return sg::Verdict::Cancel;
}

struct Watch { const ue_wrap::facts::Function* row; int tag; sg::PreFn pre; };
const Watch kWatches[] = {
    { &kRowAddObject, kTagAddObject, &OnAddObjectPre },
    { &kRowDoorOpen, kTagDoorOpen, &OnDoorOpenPre },
    { &kRowValve, kTagValve, &OnValvePre },
    { &kRowSpit, kTagSpit, &OnSpitPre },
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
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, kWatches[i].pre, nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[CREMATOR-GUARD] the script-body gate refused the watch on '%ls' of '%ls' -- for the rest of this "
                "process a client's cremator copy is not held there", kWatches[i].row->Name(), kWatches[i].row->Class());
    }
}

}  // namespace

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
        UE_LOGI("[CREMATOR-GUARD] the client guard's gates are live: addObject, a lid's open, the cremator's press and its fireball timer");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[CREMATOR-GUARD] %d of %d client guard gates are dead (refused, or resolved into a full table) -- a "
                "client's cremator copy is not held there", kWatchCount - live, kWatchCount);
    }
}

uint64_t AddObjectRefused() { return g_collectRefused; }

void OnDisconnect() {
    if (g_collectRefused || g_doorRefused || g_valveRefused || g_doorLoadNative || g_spitRefused)
        UE_LOGI("[CREMATOR-GUARD] session end -- a client refused %llu addObject call(s) on %zu cremator copy(ies), %llu "
                "own open(s) of a running cremator's door (%llu of its world's load run natively), %llu valve press(es) "
                "in %llu burst(s), %llu fireball timer call(s)", static_cast<unsigned long long>(g_collectRefused),
                g_collectSaid.size(), static_cast<unsigned long long>(g_doorRefused),
                static_cast<unsigned long long>(g_doorLoadNative), static_cast<unsigned long long>(g_valveRefused),
                static_cast<unsigned long long>(g_valveBursts), static_cast<unsigned long long>(g_spitRefused));
    g_collectRefused = g_doorRefused = g_valveRefused = g_valveBursts = g_doorLoadNative = g_spitRefused = 0;
    g_valveLastMs = 0;
    g_valveEver = false;
    g_actionSaid = false;
    g_spitSaid = false;
    g_collectSaid.clear();
}

}  // namespace coop::cremator_client_guard
