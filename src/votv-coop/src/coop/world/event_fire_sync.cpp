// coop/world/event_fire_sync.cpp -- see coop/world/event_fire_sync.h. The bytecode facts this
// module stands on: the save slot's settime iterates allEvents, skips rows in passEvents, and
// on a clock-cross fire calls the eventer's runEvent for the row and appends it to passEvents,
// so a runEvent whose caller is settime is exactly a scheduler fire, and an empty allEvents
// kills the walk (the client suppression seam); runEvent is the only function of that name,
// called from settime, from the game's own event menu and through our reflected dispatch; the
// gamemode's boot marks the rows before a new game's start day passed without firing them, and
// rebuilds allEvents from the events table on every world load, so the zeroed count self-heals
// and a client-written save cannot be poisoned; and the only special the table uses is the
// prank roll, which summonArirPrank resolves through an internal runSpecialEvent call -- so the
// watch sits on BOTH eventer verbs and the wire carries the name each call itself took (the
// rolled case, not the prank verb).

#include "coop/world/event_fire_sync.h"

#include "event_fire_policy.h"  // ReplayVerdict (co-located private header)

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/session_serial.h"
#include "coop/world/time_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>

namespace coop::event_fire_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolution, on demand, game thread: the three classes come from the object index, which answers at
// once whether or not they are loaded; their members resolve once all three are, on the first pass or
// never (a renamed symbol on a future game version), so a failed pass latches loudly.
int32_t g_offSaveSlot = -1;           // mainGamemode.saveSlot (UsaveSlot_C*)
int32_t g_offEventer = -1;            // mainGamemode.eventer  (Atrigger_eventer_C*)
int32_t g_offPassEvents = -1;         // saveSlot.passEvents (TArray<FName>)
int32_t g_offAllEvents = -1;          // saveSlot.allEvents  (TArray<FName>)
void* g_runEventFn = nullptr;         // runEvent(FName event, FName special)
void* g_runSpecialEventFn = nullptr;  // runSpecialEvent(FName eventName1) -> bool
void* g_summonArirPrankFn = nullptr;  // summonArirPrank(), runSpecialEvent's prank-roll caller
void* g_settimeFn = nullptr;          // saveSlot.settime, the scheduler's walk
int32_t g_offEventParam = -1;         // runEvent's `event` in its parameter frame
int32_t g_offSpecialParam = -1;       // runSpecialEvent's `eventName1` in its parameter frame
bool g_resolved = false;
bool g_resolveFailed = false;

// The host's watches on both eventer verbs, registered once per process. One emit per
// committed call: the watches are the ONLY broadcast source (a dev HostFire's reflected call
// reaches them like any other). There is no occurrence identity on the wire: #N in the log is
// a sequence counter only, a RunEvent dedupes on the client by name within the session, and a
// special is re-fired every broadcast by design.
constexpr int kTagEventFire = 0x45564652;  // 'EVFR'
constexpr const wchar_t* kRunEvent = L"runEvent";  // one pointer: the gate matches a name watch by it
constexpr const wchar_t* kRunSpecialEvent = L"runSpecialEvent";
bool g_watchInstalled = false;
bool g_watchLive = false;
bool g_watchDead = false;          // settled without going live: said once, no longer polled
bool g_specialWatchInstalled = false;
bool g_specialWatchLive = false;
bool g_specialWatchDead = false;
unsigned g_hostFireSeq = 0;  // committed host fires across both verbs (game thread)

// Client suppression and replay state, game thread.
int g_zeroedAllEventsNum = 0;         // what we zeroed (restore on disconnect); 0 = nothing zeroed
void* g_zeroedSaveSlot = nullptr;
int32_t g_zeroedSaveSlotIdx = -1;
struct PendingFire {
    FireKind kind;
    std::string name;
    // The active override: the host registry says this row is in flight, so bypass the
    // passEvents dedupe (a mid-event joiner's blob carries the row as completed history while
    // the event is still running).
    bool activeOverride = false;
    bool attempted = false;    // one replay try ran (paces the next)
    std::chrono::steady_clock::time_point lastAttempt{};
};
std::deque<PendingFire> g_pending;    // replays waiting for the eventer (join window)
// EventFire is pre-world-sendable, so a joiner can queue fires for its whole load window. The
// cap is the table plus specials plus margin; duplicates are skipped at queue time, so it is
// effectively unreachable.
constexpr size_t kMaxPending = 128;
// A row whose world or eventer is not up yet is retried on a real-time cadence: the per-tick
// install pump drives the drain, so a waiting row never waits for the next fire.
constexpr auto kReplayRetrySpacing = std::chrono::seconds(1);
std::unordered_set<std::string> g_replayed;  // rows replayed this session (dedupe)
std::atomic<unsigned> g_replays{0};          // fires replayed natively (ReplayCount)
// The queue and the replayed set belong to one world: unbound while a join window's fires wait
// for the first Gameplay eventer to exist (a fire for the world being loaded must not die with
// the one being left), bound to that world's generation once a replay can run, and dropped when
// the world leaves it -- a connected travel, a load, a quit to menu.
uint32_t g_boundGen = 0;  // ue_wrap::world_identity::Generation() stamp; 0 = unbound join window

// The UE4 array header; 8-byte FName elements for the two arrays touched.
struct RawArray {
    R::FName* Data;
    int32_t Num;
    int32_t Max;
};

bool MembersMissing() {
    return g_offSaveSlot < 0 || g_offEventer < 0 || g_offPassEvents < 0 || g_offAllEvents < 0 ||
           !g_runEventFn || !g_runSpecialEventFn || !g_settimeFn || g_offEventParam < 0 ||
           g_offSpecialParam < 0;
}

bool ResolvePass() {
    if (g_resolved) return true;
    if (g_resolveFailed) return false;
    namespace OI = ue_wrap::object_index;
    void* gmCls = OI::ClassByName(L"mainGamemode_C");
    void* ssCls = OI::ClassByName(L"saveSlot_C");
    void* evCls = OI::ClassByName(L"trigger_eventer_C");
    if (!gmCls || !ssCls || !evCls) return false;  // the world has not loaded them yet
    g_offSaveSlot = R::FindPropertyOffset(gmCls, L"saveSlot");
    g_offEventer = R::FindPropertyOffset(gmCls, L"eventer");
    g_offPassEvents = R::FindPropertyOffset(ssCls, L"passEvents");
    if (g_offAllEvents < 0) g_offAllEvents = R::FindPropertyOffset(ssCls, L"allEvents");
    g_runEventFn = R::FindFunction(evCls, L"runEvent");
    g_runSpecialEventFn = R::FindFunction(evCls, L"runSpecialEvent");
    // The prank roll's frame is an origin label, not a requirement: its absence costs only
    // the "prank-roll" tag in the fire log, so it stays out of the latching check.
    g_summonArirPrankFn = R::FindFunction(evCls, L"summonArirPrank");
    g_settimeFn = R::FindFunction(ssCls, L"settime");
    g_offEventParam = g_runEventFn ? R::FindParamOffset(g_runEventFn, L"event") : -1;
    g_offSpecialParam = g_runSpecialEventFn ? R::FindParamOffset(g_runSpecialEventFn, L"eventName1") : -1;
    if (MembersMissing()) {
        g_resolveFailed = true;
        UE_LOGW("event_fire: resolution INCOMPLETE on loaded classes (saveSlot=0x%X eventer=0x%X "
                "passEvents=0x%X allEvents=0x%X runEvent=%s(event=0x%X) "
                "runSpecialEvent=%s(eventName1=0x%X) settime=%s) -- "
                "latched OFF; game version mismatch?",
                g_offSaveSlot, g_offEventer, g_offPassEvents, g_offAllEvents, g_runEventFn ? "yes" : "NO",
                g_offEventParam, g_runSpecialEventFn ? "yes" : "NO", g_offSpecialParam,
                g_settimeFn ? "yes" : "NO");
        return false;
    }
    g_resolved = true;
    UE_LOGI("event_fire: resolved (saveSlot=0x%X passEvents=0x%X allEvents=0x%X eventer=0x%X "
            "runEvent=yes runSpecialEvent=yes summonArirPrank=%s settime=yes)",
            g_offSaveSlot, g_offPassEvents, g_offAllEvents, g_offEventer,
            g_summonArirPrankFn ? "yes" : "NO");
    return true;
}

void* SaveSlotOf(void* gm) {
    if (!gm || g_offSaveSlot < 0) return nullptr;
    void* ss = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSaveSlot);
    return (ss && R::IsLive(ss)) ? ss : nullptr;
}

void* EventerOf(void* gm) {
    if (!gm || g_offEventer < 0) return nullptr;
    void* ev = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offEventer);
    return (ev && R::IsLive(ev)) ? ev : nullptr;
}

RawArray* ArrayAt(void* obj, int32_t off) {
    if (!obj || off < 0) return nullptr;
    return reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(obj) + off);
}

std::string NarrowName(const R::FName& n) {
    const std::wstring w = R::ToString(n);
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

// Whether the host's watch on this verb is live in an enabled gate: a connected host reaches the
// wire only through it.
bool WatchLive(FireKind kind) {
    const wchar_t* name = kind == FireKind::SpecialEvent ? kRunSpecialEvent : kRunEvent;
    return sg::NameWatchLive(name, kTagEventFire) && sg::IsEnabled();
}

bool IsClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// A client runs no eventer verb of its own: the game's event menu and its cheat menu reach
// runEvent's and runSpecialEvent's bodies directly, and every spawn inside them is bytecode-internal,
// so a fire there mints creatures and props no mirror covers. The one eventer call a client runs is
// this module's replay, admitted by NativeFire's own scope: the eventer and the verb it dispatches,
// once. An eventer call nested inside it is refused -- the prank roll's own runSpecialEvent among
// them, whose chosen case the host sends as a fire of its own.
struct Admission {
    void* object = nullptr;
    void* function = nullptr;
    bool spent = false;
};
Admission* g_admission = nullptr;   // game thread; set for exactly the length of NativeFire's Call
uint64_t g_clientRefused = 0;

sg::Verdict OnEventerVerbPre(const sg::Call& call) {
    if (!IsClientSession()) return sg::Verdict::Run;
    Admission* a = g_admission;
    if (a && !a->spent && call.object == a->object && call.function == a->function) {
        a->spent = true;
        return sg::Verdict::Run;
    }
    const uint64_t n = ++g_clientRefused;
    if ((n & (n - 1)) == 0) {
        const bool special = call.function == g_runSpecialEventFn;
        const int32_t off = special ? g_offSpecialParam : g_offEventParam;
        const std::string ev = (call.locals && off >= 0)
            ? NarrowName(*reinterpret_cast<const R::FName*>(call.locals + off)) : std::string("?");
        UE_LOGI("event_fire: refused a client's own %s('%s') (#%llu) -- the host fires every event",
                special ? "runSpecialEvent" : "runEvent", ev.c_str(), static_cast<unsigned long long>(n));
    }
    return sg::Verdict::Cancel;
}

// The native fire, game thread. A dispatch that faults is said and not re-run.
bool NativeFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    void* eventer = EventerOf(ue_wrap::world_singleton::Gamemode());
    if (!eventer) {
        UE_LOGW("event_fire: no live trigger_eventer -- native fire dropped ('%ls')", eventName.c_str());
        return false;
    }
    const char* verb = kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent";
    void* fn = kind == FireKind::SpecialEvent ? g_runSpecialEventFn : g_runEventFn;
    if (!fn) { UE_LOGW("event_fire: %s unresolved", verb); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    // A missed Set leaves a half-written frame the body reads garbage from: do not dispatch.
    const R::FName ev = ue_wrap::fname_utils::StringToFName(eventName);
    bool filled;
    if (kind == FireKind::SpecialEvent) {
        filled = f.Set<R::FName>(L"eventName1", ev);
    } else {
        filled = f.Set<R::FName>(L"event", ev) &&
                 f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(specialName));
    }
    if (!filled) {
        UE_LOGW("event_fire: %s('%ls') param frame would not fill -- dispatch dropped",
                verb, eventName.c_str());
        return false;
    }
    Admission adm{eventer, fn};
    struct Scope {
        Admission* prev;
        explicit Scope(Admission* a) : prev(g_admission) { g_admission = a; }
        ~Scope() { g_admission = prev; }
    } scope(&adm);
    if (!ue_wrap::Call(eventer, f)) {
        UE_LOGW("event_fire: %s('%ls') dispatch FAILED", verb, eventName.c_str());
        return false;
    }
    // On a client the gate admits this call and nothing else; a live watch that did not admit it
    // refused it, so the body did not run.
    if (IsClientSession() && WatchLive(kind) && !adm.spent) {
        UE_LOGE("event_fire: %s('%ls') was refused at the gate -- the admission did not match the "
                "call; not retried", verb, eventName.c_str());
        return false;
    }
    if (kind == FireKind::SpecialEvent)
        UE_LOGI("event_fire: runSpecialEvent('%ls') dispatched", eventName.c_str());
    else
        UE_LOGI("event_fire: runEvent('%ls', special='%ls') dispatched",
                eventName.c_str(), specialName.c_str());
    return true;
}

// The resolve latched each verb's function once; a watched call through another function (a
// reloaded eventer class) is not announced, so it is said once rather than lost silently.
void SayUnlatched(const char* verb, void* fn) {
    static bool s_said = false;
    if (s_said) return;
    s_said = true;
    UE_LOGW("event_fire: a host %s ran through function %p, not the one resolved -- that fire is NOT "
            "broadcast; the eventer class was reloaded?", verb, fn);
}

void Broadcast(FireKind kind, const std::string& name) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    coop::net::EventFirePayload p{};  // zero-init -> name[] pre-NUL-bound
    p.dispatch = static_cast<uint8_t>(kind);
    const size_t n = name.size() < sizeof(p.name) - 1 ? name.size() : sizeof(p.name) - 1;
    std::memcpy(p.name, name.c_str(), n);
    s->SendReliable(coop::net::ReliableKind::EventFire, &p, sizeof(p));
    UE_LOGI("event_fire: broadcast %s '%s'",
            kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", name.c_str());
}

// The one emit point for host fires, at the body commit: runEvent entered from settime is a
// scheduler fire, a call with no Blueprint caller and fromOurCode is our own HostFire dispatch,
// and any other caller is the game's own menus -- every one reached clients before only if the
// host broadcast it, which is now this watch's job alone. POST so a cancelled body can never
// announce a fire that did not run; a client replay's own call early-outs on the role gate.
void OnRunEventPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolvePass()) return;
    if (call.function != g_runEventFn) { SayUnlatched("runEvent", call.function); return; }
    const std::string name = NarrowName(*reinterpret_cast<const R::FName*>(call.locals + g_offEventParam));
    const char* origin = call.callerFunction == g_settimeFn ? "scheduler" :
                         call.fromOurCode ? "dev-call" : "native";
    ++g_hostFireSeq;
    UE_LOGI("event_fire: host fire #%u runEvent('%s') origin=%s -- broadcasting",
            g_hostFireSeq, name.c_str(), origin);
    Broadcast(FireKind::RunEvent, name);
}

// HOST: a runSpecialEvent body just completed. The natural entry is a scheduled ariralPrank
// row: runEvent -> summonArirPrank (a rep-tier Array_Random pick, removed from the pool on the
// way out) -> runSpecialEvent, so THIS call is where the rolled outcome becomes known -- the
// row's own broadcast carries only the arirInteraction name. Dev fires (reflected, no caller)
// and the game's cheat menu reach it the same way and emit through here exactly once.
void OnRunSpecialEventPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolvePass()) return;
    if (call.function != g_runSpecialEventFn) { SayUnlatched("runSpecialEvent", call.function); return; }
    const std::string name =
        NarrowName(*reinterpret_cast<const R::FName*>(call.locals + g_offSpecialParam));
    const char* origin =
        (g_summonArirPrankFn && call.callerFunction == g_summonArirPrankFn) ? "prank-roll" :
        call.fromOurCode ? "dev-call" : "native";
    ++g_hostFireSeq;
    UE_LOGI("event_fire: host fire #%u runSpecialEvent('%s') origin=%s -- broadcasting",
            g_hostFireSeq, name.c_str(), origin);
    Broadcast(FireKind::SpecialEvent, name);
}

// True iff the client's own passEvents already contains the row (the transferred save
// carried this fire; its world effects are already in the loaded state).
bool InClientPassEvents(const std::string& name) {
    RawArray* pass = ArrayAt(SaveSlotOf(ue_wrap::world_singleton::Gamemode()), g_offPassEvents);
    if (!pass || !pass->Data || pass->Num <= 0 || pass->Num > 100000) return false;
    std::wstring w(name.begin(), name.end());
    const R::FName want = ue_wrap::fname_utils::StringToFName(w);
    for (int32_t i = 0; i < pass->Num; ++i) {
        const R::FName& e = pass->Data[i];
        if (e.ComparisonIndex == want.ComparisonIndex && e.Number == want.Number) return true;
    }
    return false;
}

// The replay step's result, game thread: Done pops the row, NotReady waits it (uncounted and paced),
// PermFailed drops it -- the event classes can never resolve on this build.
enum class ReplayStep { Done, NotReady, PermFailed };

// A terminal step drops the row outright and says why once, here.
bool DropIfTerminal(ReplayStep step, const PendingFire& pf) {
    if (step != ReplayStep::PermFailed) return false;
    UE_LOGE("event_fire: replay of '%s' DROPPED -- the event classes never resolved "
            "(permanent failure); the row could never dispatch", pf.name.c_str());
    return true;
}

// The client replay executor, game thread.
ReplayStep TryReplay(const PendingFire& pf) {
    if (!ResolvePass() || !EventerOf(ue_wrap::world_singleton::Gamemode()))
        return g_resolveFailed ? ReplayStep::PermFailed : ReplayStep::NotReady;
    // A replay dispatches only into the CURRENT gameplay world: while it is Unknown or
    // another, the singleton lookup can still answer the eventer of the world being left
    // (the cached ref fails open on a null current world).
    if (ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay)
        return ReplayStep::NotReady;
    if (g_boundGen == 0) g_boundGen = ue_wrap::world_identity::Generation();
    // Dedupe applies to one-shot scheduled rows only (the game's own passEvents semantics);
    // specials (graffiti, pranks the menu re-fires) are repeatable by design.
    if (pf.kind == FireKind::RunEvent) {
        if (g_replayed.count(pf.name)) {
            UE_LOGI("event_fire: '%s' already replayed this session -- skipping", pf.name.c_str());
            return ReplayStep::Done;
        }
        // A passEvents hit marks nothing: the skip must stay re-decidable, since marking here would
        // let a history-skipped fire permanently block a later in-flight override for the same row
        // (a fire landing between the joiner's connect and its blob capture rides both the wire and
        // the blob). A duplicate just rescans the array; passEvents never shrinks mid-session.
        if (!pf.activeOverride && InClientPassEvents(pf.name)) {
            UE_LOGI("event_fire: '%s' already in local passEvents (save carried it) -- skipping",
                    pf.name.c_str());
            return ReplayStep::Done;
        }
    }
    const std::wstring w(pf.name.begin(), pf.name.end());
    UE_LOGI("event_fire: client REPLAY %s '%s'%s",
            pf.kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", pf.name.c_str(),
            pf.activeOverride ? " (in-flight active-override)" : "");
    // The special is always None: the only native special is the prank roll, whose chosen case
    // the host's runSpecialEvent watch sends on its own. Marked consumed only on a successful
    // dispatch (a frame or call failure is loud and must not eat the row).
    if (NativeFire(pf.kind, w, L"None")) {
        g_replays.fetch_add(1, std::memory_order_relaxed);
        if (pf.kind == FireKind::RunEvent) g_replayed.insert(pf.name);
    }
    return ReplayStep::Done;
}

// A queued row that could not run yet: stamp the pacing of its next try.
void NoteAttempt(PendingFire& pf) {
    pf.attempted = true;
    pf.lastAttempt = std::chrono::steady_clock::now();
}

// Before a drain and before a new fire is queued: rows and dedupe bound to a world since left
// describe it, not this one, so they close out here. An unbound queue (the join window) has no
// world to be stale in and is left alone, as is the queue through the travel itself -- the
// generation only moves once the new world is observed.
void DropStaleWorldQueue() {
    if (g_boundGen == 0) return;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_boundGen) return;
    UE_LOGI("event_fire: world changed (gen %u -> %u) -- dropped %zu queued fire(s) and cleared "
            "the replayed set (%zu entr(ies)); the new join queue stays unbound",
            g_boundGen, gen, g_pending.size(), g_replayed.size());
    g_pending.clear();
    g_replayed.clear();
    g_boundGen = 0;
}

// CLIENT: replay the queued fires in arrival order, stopping at the first the eventer cannot take
// yet; a front row that already tried waits out its spacing first. Run before a new fire is
// handled, at the client's world-ready announce, and every install tick (the lasting retry).
void DrainPending() {
    DropStaleWorldQueue();
    const auto now = std::chrono::steady_clock::now();
    while (!g_pending.empty()) {
        PendingFire& front = g_pending.front();
        // A latched resolve failure is permanent -- no dispatch is possible on this build, so
        // the row drops outright instead of wedging the FIFO behind a forever NotReady, with no
        // spacing to wait out first.
        if (front.attempted && !g_resolveFailed && now - front.lastAttempt < kReplayRetrySpacing)
            return;
        const ReplayStep step = TryReplay(front);
        if (step == ReplayStep::Done) {
            g_pending.pop_front();
            continue;
        }
        if (DropIfTerminal(step, front)) {
            g_pending.pop_front();
            continue;
        }
        NoteAttempt(front);
        return;
    }
}

// CLIENT: the cycle is about to tick, and its settime walks the save's list of events on any clock
// change: hold the list empty before the tick's body runs, so no row is ever due on a client -- the
// first tick of a new world included, where the clock lane's pre-observer writes the host's newest
// sample into the same tick. Held on exactly the cycles the clock lane parks, by its own predicate.
bool g_tickObserved = false;  // the pre-observer is registered (once per process)
bool g_holdUnresolvable = false;  // the save slot's class has no allEvents; said once

void OnCycleTickPre(void* self, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread() || !coop::time_sync::HoldsCycle(self)) return;
    void* ss = ue_wrap::daynightcycle::SaveSlotOfCycle(self);
    if (!ss) return;
    // The list's offset from the live slot's own class, a property lookup with no object-array walk,
    // so the first tick of a world is held even before the other members have resolved.
    if (g_offAllEvents < 0) {
        if (g_holdUnresolvable) return;
        g_offAllEvents = R::FindPropertyOffset(R::ClassOf(ss), L"allEvents");
        if (g_offAllEvents < 0) {
            g_holdUnresolvable = true;
            UE_LOGW("event_fire: the save slot has no allEvents -- a client's event walk cannot be held");
            return;
        }
    }
    RawArray* all = ArrayAt(ss, g_offAllEvents);
    if (!all || all->Num <= 0 || all->Num > 100000) return;  // 0 = already suppressed
    // A legal array state (empty with slack): data and capacity untouched, and the engine frees
    // the same allocation later. The gamemode's boot rebuilds allEvents from the table on every
    // world load, so this re-asserts after any reload and can never poison a save.
    g_zeroedAllEventsNum = all->Num;
    g_zeroedSaveSlot = ss;
    g_zeroedSaveSlotIdx = R::InternalIndexOf(ss);
    all->Num = 0;
    UE_LOGI("event_fire: client scheduler SUPPRESSED at the cycle's tick (allEvents %d -> 0; host is the only "
            "firer; restored on disconnect)", g_zeroedAllEventsNum);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Called every pump tick by the install fanout, which is also the retry until the cycle class
    // loads and until the gate has resolved the watch's name.
    // The PRE is the client's refusal (OnEventerVerbPre), the POST the host's emit.
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRunEvent, kTagEventFire, &OnEventerVerbPre, &OnRunEventPost);
    if (!g_specialWatchInstalled)
        g_specialWatchInstalled =
            sg::WatchName(kRunSpecialEvent, kTagEventFire, &OnEventerVerbPre, &OnRunSpecialEventPost);
    const bool runPending = g_watchInstalled && !g_watchLive && !g_watchDead;
    const bool specialPending = g_specialWatchInstalled && !g_specialWatchLive && !g_specialWatchDead;
    if (runPending || specialPending) sg::ResolvePendingNames();
    if (runPending) {
        if (sg::NameWatchLive(kRunEvent, kTagEventFire)) {
            g_watchLive = true;
            UE_LOGI("event_fire: the host's fires are seen at runEvent (a script-gate watch)");
        } else if (sg::NameWatchSettled(kRunEvent, kTagEventFire)) {
            g_watchDead = true;
            UE_LOGE("event_fire: the runEvent watch settled dead -- a host's fires do not cross, and "
                    "a client's own are not refused, this session");
        }
    }
    if (specialPending) {
        if (sg::NameWatchLive(kRunSpecialEvent, kTagEventFire)) {
            g_specialWatchLive = true;
            UE_LOGI("event_fire: the host's special picks are seen at runSpecialEvent (a script-gate watch)");
        } else if (sg::NameWatchSettled(kRunSpecialEvent, kTagEventFire)) {
            g_specialWatchDead = true;
            UE_LOGE("event_fire: the runSpecialEvent watch settled dead -- a host's specials do not cross, "
                    "and a client's own are not refused, this session");
        }
    }
    // The client replay queue's lasting retry: a fire whose world or eventer was not up no longer
    // waits for the next fire to arrive -- this pump paces it instead.
    if (!g_pending.empty()) DrainPending();
    namespace DNC = ue_wrap::daynightcycle;
    if (g_tickObserved || !DNC::EnsureResolved()) return;
    void* fn = DNC::TickFunction();
    if (!fn) {
        UE_LOGW("event_fire: daynightCycle_C::ReceiveTick not found -- a client's event walk cannot be held");
        g_tickObserved = true;
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnCycleTickPre)) {
        static bool s_said = false;  // retried every pump tick; said once
        if (!s_said) UE_LOGW("event_fire: the cycle tick's pre-observer did not register (table full?) -- retrying");
        s_said = true;
        return;
    }
    g_tickObserved = true;
    UE_LOGI("event_fire: a client's event walk is held at the cycle's own tick (pre-observer on ReceiveTick)");
}

bool HostFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->running() && s->role() != coop::net::Role::Host) {
        UE_LOGW("event_fire: HostFire refused -- running as a client (host is authoritative)");
        return false;
    }
    // Session objects can be reused without a world change. Stamp the start as well as the pointer.
    const uint32_t gen = ue_wrap::world_identity::Generation();
    const uint32_t serial = coop::net::session_serial::Current();
    const bool wasRunning = s && s->running();
    const bool wasConnectedHost = s && s->connected() && s->role() == coop::net::Role::Host;
    const std::wstring ev = eventName;
    const std::wstring sp = specialName.empty() ? L"None" : specialName;
    GT::Post([kind, ev, sp, s, gen, serial, wasRunning, wasConnectedHost] {
        auto* cur = g_session.load(std::memory_order_acquire);
        if (cur && cur->running() && cur->role() != coop::net::Role::Host) {
            UE_LOGW("event_fire: HostFire('%ls') dropped -- the session is a running CLIENT now",
                    ev.c_str());
            return;
        }
        if (cur != s || ue_wrap::world_identity::Generation() != gen ||
            coop::net::session_serial::Current() != serial) {
            UE_LOGW("event_fire: HostFire('%ls') dropped -- the world or the session changed "
                    "between submit and dispatch", ev.c_str());
            return;
        }
        // A fire submitted by a starting or connected host dies with that session.
        if ((wasRunning && !(cur && cur->running())) ||
            (wasConnectedHost && !(cur && cur->connected()))) {
            UE_LOGW("event_fire: HostFire('%ls') dropped -- the submitting host session ended "
                    "before dispatch", ev.c_str());
            return;
        }
        // Resolve here, not before the post: a solo host's dev menu has no session. The native fire
        // warns loudly if the world or the eventer is not up.
        if (!ResolvePass()) {
            UE_LOGW("event_fire: HostFire('%ls') -- the event classes are not loaded", ev.c_str());
            return;
        }
        // A connected host reaches the wire only through the watches, so an unobserved fire
        // would run the event and tell nobody -- it is refused rather than waited out. Solo
        // keeps the unobserved dispatch path.
        if (cur && cur->connected() && !WatchLive(kind)) {
            UE_LOGW("event_fire: HostFire('%ls') dropped -- the %s watch is not live (a connected "
                    "host fires only through it)", ev.c_str(),
                    kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent");
            return;
        }
        // No send of its own: the runEvent/runSpecialEvent watches see this reflected call like
        // any other host fire and emit it once, after its body (a dev fire is the 'dev-call' origin).
        NativeFire(kind, ev, sp);
    });
    return true;
}

void OnReliable(const coop::net::EventFirePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: OnReliable off-game-thread -- dropping"); return; }
    // NUL-bound the name (the payload crosses the trust boundary; the dispatcher length-checked
    // it).
    char buf[sizeof(payload.name) + 1] = {};
    std::memcpy(buf, payload.name, sizeof(payload.name));
    const std::string name(buf);
    if (name.empty() || payload.dispatch > static_cast<uint8_t>(FireKind::SpecialEvent)) {
        UE_LOGW("event_fire: OnReliable malformed (dispatch=%u, name='%s') -- dropping",
                payload.dispatch, name.c_str());
        return;
    }
    const FireKind kind = static_cast<FireKind>(payload.dispatch);
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(name, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: '%s' NOT replayed (%s)", name.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: '%s' not in the replay policy -- default NO-replay (newer host? "
                "add the row's verdict)", name.c_str());
        return;
    }
    DrainPending();
    PendingFire pf{ kind, name };
    if (g_pending.empty()) {  // a non-empty queue tries it in FIFO order later
        const ReplayStep step = TryReplay(pf);
        if (step == ReplayStep::Done) return;
        if (DropIfTerminal(step, pf)) return;
        NoteAttempt(pf);  // the queue must remember the try: it paces the retry
    }
    // One-shot rows dedupe at queue time too (a scheduler re-fire of a dev-fired row during the
    // same load window would otherwise queue twice; the replay would catch it later, but a
    // duplicate-free queue keeps the cap honest).
    if (kind == FireKind::RunEvent) {
        for (const auto& q : g_pending)
            if (q.kind == kind && q.name == name) return;
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping '%s'",
                g_pending.size(), name.c_str());
        return;
    }
    UE_LOGI("event_fire: queued '%s' (%zu pending; the world or eventer is not up yet)",
            name.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void ReplayInFlightRow(const std::string& rowName) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: ReplayInFlightRow off-game-thread -- dropping"); return; }
    if (rowName.empty()) return;
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(rowName, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: in-flight '%s' NOT replayed (%s)", rowName.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: in-flight '%s' not in the replay policy -- default NO-replay "
                "(add the row's verdict)", rowName.c_str());
        return;
    }
    DrainPending();
    PendingFire pf{ FireKind::RunEvent, rowName, /*activeOverride=*/true };
    if (g_pending.empty()) {
        const ReplayStep step = TryReplay(pf);
        if (step == ReplayStep::Done) return;
        if (DropIfTerminal(step, pf)) return;
        NoteAttempt(pf);  // the queue must remember the try: it paces the retry
    }
    // The world or eventer is not up yet. If the row is already queued (a fire copy
    // landed in the pre-world window), upgrade it in place: two entries would double-dispatch,
    // and the plain copy alone could history-skip the in-flight replay.
    for (auto& q : g_pending) {
        if (q.kind == FireKind::RunEvent && q.name == rowName) {
            q.activeOverride = true;
            UE_LOGI("event_fire: in-flight '%s' already queued -- upgraded to active-override",
                    rowName.c_str());
            return;
        }
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping in-flight '%s'",
                g_pending.size(), rowName.c_str());
        return;
    }
    UE_LOGI("event_fire: queued in-flight '%s' (%zu pending)",
            rowName.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void OnClientWorldReady() {
    if (g_pending.empty()) return;
    const size_t before = g_pending.size();
    DrainPending();
    if (g_pending.empty())
        UE_LOGI("event_fire: world ready -- the %zu queued fire(s) replayed or skipped", before);
    else
        UE_LOGI("event_fire: world ready with %zu of %zu queued fire(s) still pending -- "
                "the install pump retries them each second", g_pending.size(), before);
}

void OnDisconnect() {
    // Restore the client's scheduler only if we zeroed this exact live save slot and nothing
    // repopulated it since (a boot rebuild leaves the count positive, and then the restore must
    // not run).
    if (g_zeroedAllEventsNum > 0 && g_zeroedSaveSlot &&
        R::IsLiveByIndex(g_zeroedSaveSlot, g_zeroedSaveSlotIdx)) {
        RawArray* all = ArrayAt(g_zeroedSaveSlot, g_offAllEvents);
        if (all && all->Num == 0 && all->Max >= g_zeroedAllEventsNum) {
            all->Num = g_zeroedAllEventsNum;
            UE_LOGI("event_fire: allEvents restored (0 -> %d) -- local scheduler resumes",
                    g_zeroedAllEventsNum);
        }
    }
    g_zeroedAllEventsNum = 0;
    g_zeroedSaveSlot = nullptr;
    g_zeroedSaveSlotIdx = -1;
    g_pending.clear();
    g_replayed.clear();
    g_boundGen = 0;
    g_session.store(nullptr, std::memory_order_release);
}

bool DevProbeClientRefusal() {
    if (!GT::IsGameThread() || !IsClientSession() || !ResolvePass()) return false;
    void* eventer = EventerOf(ue_wrap::world_singleton::Gamemode());
    if (!eventer || !g_runEventFn) return false;
    ue_wrap::ParamFrame f(g_runEventFn);
    if (!f.valid() || !f.Set<R::FName>(L"event", ue_wrap::fname_utils::StringToFName(L"solar")) ||
        !f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(L"None")))
        return false;
    // The client's own call, as the game's event menu makes it: no admission is held, so the gate
    // must refuse it.
    const uint64_t before = g_clientRefused;
    ue_wrap::Call(eventer, f);
    return g_clientRefused == before + 1;
}

unsigned ReplayCount() { return g_replays.load(std::memory_order_relaxed); }

}  // namespace coop::event_fire_sync
