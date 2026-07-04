// coop/world/event_fire_sync.cpp -- see coop/world/event_fire_sync.h.
//
// Bytecode ground truth this module stands on (verified 2026-07-03, research/bp_reflection):
//   - saveSlot::settime: iterates saveSlot.allEvents; skips rows in passEvents (Array_Contains);
//     on a clock-cross fire does Array_Add(passEvents, n) + eventer.runEvent(n, row.special).
//     -> passEvents GROWTH is exactly "the scheduler fired a row" (the host observation seam);
//     -> allEvents.Num == 0 kills the walk (the client suppression seam);
//     -> runEvent itself neither checks nor appends passEvents (replay can't self-block,
//        dev fires can't double-broadcast through the poll).
//   - mainGamemode boot ubergraph: allEvents = GetDataTableRowNames(list_events) UNCONDITIONALLY
//     every world load -> the zeroed Num self-heals; a client-written save cannot be poisoned.
//   - The only specialTrigger value in list_events is 'ariralPrank' (summonArirPrank = host-local
//     RNG); the wire deliberately carries NO special field.

#include "coop/world/event_fire_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>

namespace coop::event_fire_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- resolution (game thread; lazy, 2 s retry throttle) ------------------------------------
void* g_gmCls = nullptr;
void* g_gm = nullptr;                 // live mainGamemode_C instance
int32_t g_gmIdx = -1;
int32_t g_offSaveSlot = -1;           // mainGamemode.saveSlot (UsaveSlot_C*)
int32_t g_offEventer = -1;            // mainGamemode.eventer  (Atrigger_eventer_C*)
void* g_saveSlotCls = nullptr;
int32_t g_offPassEvents = -1;         // saveSlot.passEvents (TArray<FName>)
int32_t g_offAllEvents = -1;          // saveSlot.allEvents  (TArray<FName>)
void* g_eventerCls = nullptr;
void* g_runEventFn = nullptr;         // runEvent(FName event, FName special)
void* g_runSpecialEventFn = nullptr;  // runSpecialEvent(FName eventName1) -> bool
std::chrono::steady_clock::time_point g_nextResolve{};
bool g_loggedResolved = false;

// ---- host poll state (game thread) ----------------------------------------------------------
void* g_polledSaveSlot = nullptr;     // the instance the baseline belongs to
int32_t g_polledSaveSlotIdx = -1;
int g_passBaseline = -1;              // -1 = prime on next successful read (never broadcast)
long long g_lastPollMs = 0;
constexpr long long kPollIntervalMs = 1000;  // scheduler fires are minutes apart; 1 Hz is generous

// ---- client suppression + replay state (game thread) ----------------------------------------
int g_zeroedAllEventsNum = 0;         // what we zeroed (restore on disconnect); 0 = nothing zeroed
void* g_zeroedSaveSlot = nullptr;
int32_t g_zeroedSaveSlotIdx = -1;
struct PendingFire {
    FireKind kind;
    std::string name;
    // v98 active-override (ReplayInFlightRow): the host registry says this row is IN FLIGHT --
    // bypass the InClientPassEvents dedupe (a mid-event joiner's blob carries the row as
    // "completed history" while the event is still running; COOP_EVENT_JOIN.md section 2).
    bool activeOverride = false;
};
std::deque<PendingFire> g_pending;    // replays waiting for the eventer (join window)
// EventFire is pre-world-sendable (session_lanes.h) -- a joiner can queue fires for its whole
// 30-60 s load window. 128 = the 69-row table + specials + margin; duplicates are skipped at
// queue time (below), so the cap is effectively unreachable.
constexpr size_t kMaxPending = 128;
std::unordered_set<std::string> g_replayed;  // rows replayed this session (dedupe)

// UE4 TArray header (8-byte FName elements for the two arrays we touch;
// FName = {ComparisonIndex, Number} -- game's own struct_event.hpp proves 8 B).
struct RawArray {
    R::FName* Data;
    int32_t Num;
    int32_t Max;
};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- the REPLAY POLICY (the dupe matrix) -----------------------------------------------------
// Ground truth: votv-event-system-RE-2026-06-13.md section 10 + 10.4 (every runEvent case's
// concrete output class + which lane already carries it). DEFAULT = NO-replay (safe: today's
// behavior). Replay ONLY rows whose effect is a deterministic level/save/cosmetic flip that NO
// existing lane delivers -- replaying a lane-covered row would double-deliver (client-local dup).
// Keyed by NAME only: the few names living in both dispatchers (falseEnter/mann/crys/fakeGrays)
// have the same verdict either way; the replay CALL still uses the received dispatch kind.
const char* const kReplayRows[] = {
    // story / save flips (level-placed triggerBase; no lane) -- the campfire target:
    "treehouse_0", "treehouse_1", "treehouse_2", "treehouse_3", "treehouse_4", "treehouse_5",
    "break_RomeoSierra", "break_Victor", "break_Victor2",
    "obelisk",
    // forceObjects appends (saveSlot array the client's own dish scan reads; no lane):
    "looker_0-1", "looker_1-1", "looker_2-1", "looker_3-1", "looker_4-1",
    "arirSignal", "arirSpk", "picSignal", "peace",
    "arirSat_0", "arirSat_1", "arirSat_2", "piramid_sig",
    // cosmetic / sound with no lane (solar's lights-dark converges with the light lane --
    // same resulting state, echo-suppressed by its lastKnown prime):
    "solar", "call0",
    // TBoxActivator scare arms -- per-viewer scares by SP design; arming BOTH sides is the
    // correct coop semantics (each player gets the scare on their own overlap):
    "toeStab", "falseEnter", "mann", "vent", "crys", "fakeGrays", "susArir",
    // graffiti decal specials (grime decal spawn; no lane):
    "arirGraff_0", "arirGraff_1", "arirGraff_2", "arirGraff_3",
    "arirGraff_4", "arirGraff_5", "arirGraff_6",
};

struct NoReplayRow { const char* name; const char* lane; };
const NoReplayRow kNoReplayRows[] = {
    // outputs already ride a lane (replay = double delivery):
    { "starRain", "event_cue lane (cue 0)" },
    { "arirFollower", "npc lane" },
    // 2026-07-03: the swarm's wisp_C rides the npc lane (EX_CallMath source-gated catch,
    // npc_world_enum) -- STILL no-replay: the lane carries the spawns; a replay would arm the
    // client's own trigger_wispSwarm and double-spawn client-local creatures on top of mirrors:
    { "wisps", "npc lane (EX-catch; event-swarm wisp_C mirrored)" },
    // 2026-07-04 verdict FLIP (sat in kReplayRows, KNOWN WRONG): the pyramid's path is
    // HOST-RANDOM (wander + wisp-chase timers), so the replay armed the client's own TB box
    // and a client walk-in spawned a DIVERGENT client-local pyramid + 4 unmirrored wisps.
    // The arrival now arrives by MIRROR: world_actor pose stream (piramid2_C allowlisted) +
    // npc-lane wisps + coop/creatures/piramid_sync (brain suppression + PyramidGather relay).
    // docs/events/piramid.md.
    { "piramid", "piramid mirror lane (WA pose + piramid_sync brain/gather)" },
    // arirShip: the TBox arm's overlap spawns arirShip_C + alarmLamp_C (+ possible ariral NPC
    // leaves) -- replaying the arm would spawn them CLIENT-LOCAL (RE 2026-06-13 #65 GAP-spawn;
    // audit M3). Host-only until the ship gets a lane:
    { "arirShip", "actor spawn on armed overlap (no lane)" },
    { "earthTp", "SELF (pose stream)" },
    { "vehtp", "atv lane" },
    { "bedEvent", "sleep lane" },
    { "picnic", "prop lane" }, { "destroyPicnic", "prop lane" },
    { "enasus", "prop lane" }, { "enacros", "prop lane" },
    { "cookier", "prop lane (armed prop)" }, { "paperGray", "prop lane (armed prop)" },
    { "arirEgg", "prop lane (armed prop)" },
    { "console", "device lanes" }, { "lightswitch", "device lanes" },
    { "keypadGuess", "device lanes" }, { "atvExplode", "atv lane (trap flag)" },
    // host-local by design:
    { "agrav", "physics divergence (by-design host-local)" },
    { "treehouseSleep", "per-player teleport" },
    // creature / save-actor spawns: host-only until allowlisted (RE 10.2). ventCrawler_C IS
    // npc-allowlisted (sdk_profile NpcClass_VentCrawler) -- its no-replay reason is MIRRORS:
    { "ventCrawler", "npc lane (allowlisted)" }, { "ventKnocker", "creature spawn (no lane yet)" },
    { "tentacleBalls", "creature spawn (no lane yet)" }, { "morningGay", "creature spawn (no lane yet)" },
    { "borgRozital", "creature spawn (no lane yet)" }, { "graysforest", "creature spawn (no lane yet)" },
    { "graystank", "creature spawn (no lane yet)" }, { "arirBuster", "creature spawn (no lane yet)" },
    { "eggvasion", "creature spawn (no lane yet)" }, { "boarwar", "creature spawn (no lane yet)" },
    { "soltoClean", "creature spawn (no lane yet)" },
    { "salt", "save-actor spawn (no lane)" }, { "rozitalHole", "save-actor spawn (no lane)" },
    { "dreambase", "save-actor spawn (no lane)" },
    { "fallbody_0", "dropper spawn (no lane)" }, { "fallbody_1", "dropper spawn (no lane)" },
    { "fallcar_0", "dropper spawn (no lane)" },
    // prank layer (host-local RNG; thrown-prop outputs ride the prop lane):
    { "food", "prank special (prop lane)" }, { "drive", "prank special (prop lane)" },
    { "atvFuel", "prank special (prop lane)" }, { "atvFix", "prank special (prop lane)" },
    { "poisonFood", "prank special (prop lane)" }, { "expDrive", "prank special (prop lane)" },
    { "cookiebox", "prank special (prop lane)" }, { "trashPiles", "prank special (prop lane)" },
    { "vaccine", "prank special (prop lane)" }, { "oil", "prank special (prop lane)" },
    { "begos", "prank special (prop lane)" }, { "gascans", "prank special (prop lane)" },
    { "bombBox", "prank special (prop lane)" },
    { "rockThrow", "prank spawner (no lane)" }, { "hillRoller", "prank spawner (no lane)" },
    { "alienJump", "prank spawner (no lane)" }, { "trashBase", "prank spawner (no lane)" },
    { "alienSounds", "sound gap (future WorldSoundCue)" },
};

// arirInteraction_0..15: the row's entire effect is the ariralPrank special (host-local RNG).
bool IsPrankRow(const std::string& n) {
    return n.rfind("arirInteraction_", 0) == 0;
}

// verdict: 1 = replay, 0 = known no-replay (*laneOut = why), -1 = unknown (default no-replay).
int ReplayVerdict(const std::string& name, const char** laneOut) {
    for (const char* r : kReplayRows)
        if (name == r) return 1;
    for (const auto& nr : kNoReplayRows)
        if (name == nr.name) { *laneOut = nr.lane; return 0; }
    if (IsPrankRow(name)) { *laneOut = "prank special (host-local RNG)"; return 0; }
    return -1;
}

// ---- resolution ------------------------------------------------------------------------------
// The three BP classes load with the world -> retried until found (the sibling-module pattern;
// bounded by menu/load time). Members on a LOADED class either resolve on the first pass or
// never (a renamed symbol on a future game version) -- capped + latched LOUD (perf-audit W-1:
// an unresolvable name must not walk GUObjectArray every 2 s for the whole session).
int g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

bool MembersMissing() {
    return g_offSaveSlot < 0 || g_offEventer < 0 || g_offPassEvents < 0 || g_offAllEvents < 0 ||
           !g_runEventFn || !g_runSpecialEventFn;
}

void ResolvePass() {
    if (g_resolveLatched) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_saveSlotCls) g_saveSlotCls = R::FindClass(L"saveSlot_C");
    if (!g_eventerCls) g_eventerCls = R::FindClass(L"trigger_eventer_C");
    if (!g_gmCls || !g_saveSlotCls || !g_eventerCls) return;  // world not loaded yet -- keep trying
    if (g_offSaveSlot < 0) g_offSaveSlot = R::FindPropertyOffset(g_gmCls, L"saveSlot");
    if (g_offEventer < 0) g_offEventer = R::FindPropertyOffset(g_gmCls, L"eventer");
    if (g_offPassEvents < 0) g_offPassEvents = R::FindPropertyOffset(g_saveSlotCls, L"passEvents");
    if (g_offAllEvents < 0) g_offAllEvents = R::FindPropertyOffset(g_saveSlotCls, L"allEvents");
    if (!g_runEventFn) g_runEventFn = R::FindFunction(g_eventerCls, L"runEvent");
    if (!g_runSpecialEventFn) g_runSpecialEventFn = R::FindFunction(g_eventerCls, L"runSpecialEvent");
    if (!MembersMissing()) {
        g_resolveLatched = true;
        g_loggedResolved = true;
        UE_LOGI("event_fire: resolved (saveSlot=0x%X passEvents=0x%X allEvents=0x%X eventer=0x%X "
                "runEvent=yes runSpecialEvent=yes)",
                g_offSaveSlot, g_offPassEvents, g_offAllEvents, g_offEventer);
        return;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;  // classes ARE loaded; a member lookup that failed 5x never succeeds
        UE_LOGW("event_fire: resolution INCOMPLETE after %d passes on loaded classes "
                "(saveSlot=0x%X eventer=0x%X passEvents=0x%X allEvents=0x%X runEvent=%s "
                "runSpecialEvent=%s) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offSaveSlot, g_offEventer, g_offPassEvents, g_offAllEvents,
                g_runEventFn ? "yes" : "NO", g_runSpecialEventFn ? "yes" : "NO");
    }
}

// Live mainGamemode instance (cached; revalidated by internal index -- freed-memory misreads).
void* Gamemode() {
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        g_gm = nullptr;
        g_gmIdx = -1;
        if (!g_gmCls) return nullptr;
        for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
                g_gm = obj;
                g_gmIdx = R::InternalIndexOf(obj);
                break;
            }
        }
    }
    return g_gm;
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

// ---- the native fire (game thread) -----------------------------------------------------------
// Returns true iff the verb actually dispatched -- callers gate the broadcast / the replayed-set
// on it (audit M1/M4: a failed host fire must not make clients replay an event the authority
// never executed, and a failed replay must not permanently consume the row for the session).
bool NativeFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    void* eventer = EventerOf(Gamemode());
    if (!eventer) {
        UE_LOGW("event_fire: no live trigger_eventer -- native fire dropped ('%ls')", eventName.c_str());
        return false;
    }
    if (kind == FireKind::SpecialEvent) {
        if (!g_runSpecialEventFn) { UE_LOGW("event_fire: runSpecialEvent unresolved"); return false; }
        ue_wrap::ParamFrame f(g_runSpecialEventFn);
        if (!f.valid()) return false;
        f.Set<R::FName>(L"eventName1", ue_wrap::fname_utils::StringToFName(eventName));
        if (ue_wrap::Call(eventer, f)) {
            UE_LOGI("event_fire: runSpecialEvent('%ls') dispatched", eventName.c_str());
            return true;
        }
        UE_LOGW("event_fire: runSpecialEvent('%ls') dispatch FAILED", eventName.c_str());
        return false;
    }
    if (!g_runEventFn) { UE_LOGW("event_fire: runEvent unresolved"); return false; }
    ue_wrap::ParamFrame f(g_runEventFn);
    if (!f.valid()) return false;
    f.Set<R::FName>(L"event", ue_wrap::fname_utils::StringToFName(eventName));
    f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(specialName));
    if (ue_wrap::Call(eventer, f)) {
        UE_LOGI("event_fire: runEvent('%ls', special='%ls') dispatched",
                eventName.c_str(), specialName.c_str());
        return true;
    }
    UE_LOGW("event_fire: runEvent('%ls') dispatch FAILED", eventName.c_str());
    return false;
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

// True iff the client's own passEvents already contains the row (the transferred save
// carried this fire -- its world effects are already in the loaded state).
bool InClientPassEvents(const std::string& name) {
    RawArray* pass = ArrayAt(SaveSlotOf(Gamemode()), g_offPassEvents);
    if (!pass || !pass->Data || pass->Num <= 0 || pass->Num > 100000) return false;
    std::wstring w(name.begin(), name.end());
    const R::FName want = ue_wrap::fname_utils::StringToFName(w);
    for (int32_t i = 0; i < pass->Num; ++i) {
        const R::FName& e = pass->Data[i];
        if (e.ComparisonIndex == want.ComparisonIndex && e.Number == want.Number) return true;
    }
    return false;
}

// Client replay executor (game thread). Returns false if the eventer isn't up yet (re-queue).
bool TryReplay(const PendingFire& pf) {
    if (!EventerOf(Gamemode())) return false;
    // Dedupe applies to ONE-SHOT scheduled rows only (the game's own passEvents semantics).
    // Specials (graffiti, pranks the menu re-fires) are repeatable by design -- replay each time.
    if (pf.kind == FireKind::RunEvent) {
        if (g_replayed.count(pf.name)) {
            UE_LOGI("event_fire: '%s' already replayed this session -- skipping", pf.name.c_str());
            return true;
        }
        // A passEvents hit marks nothing (v98): the skip must stay re-decidable. Inserting into
        // g_replayed here would let a history-skipped EventFire permanently block a LATER
        // EventSnapshot active-override for the same in-flight row (fire lands between the
        // joiner's connect and its blob capture -> both the wire copy AND the blob carry it).
        // A future duplicate just rescans the array -- passEvents never shrinks mid-session.
        if (!pf.activeOverride && InClientPassEvents(pf.name)) {
            UE_LOGI("event_fire: '%s' already in local passEvents (save carried it) -- skipping",
                    pf.name.c_str());
            return true;
        }
    }
    const std::wstring w(pf.name.begin(), pf.name.end());
    UE_LOGI("event_fire: client REPLAY %s '%s'%s",
            pf.kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", pf.name.c_str(),
            pf.activeOverride ? " (in-flight active-override)" : "");
    // special = None ALWAYS: the only native special is ariralPrank (host-local RNG roll --
    // replaying it would roll a DIFFERENT prank on this peer). Mark consumed only on a
    // SUCCESSFUL dispatch (a ParamFrame/Call failure is loud + must not eat the row).
    if (NativeFire(pf.kind, w, L"None") && pf.kind == FireKind::RunEvent)
        g_replayed.insert(pf.name);
    return true;
}

// ---- host poll / client suppress ticks (game thread, throttled by the caller) ----------------
void HostPollTick() {
    void* ss = SaveSlotOf(Gamemode());
    if (!ss || g_offPassEvents < 0) return;
    const int32_t ssIdx = R::InternalIndexOf(ss);
    RawArray* pass = ArrayAt(ss, g_offPassEvents);
    if (!pass || pass->Num < 0 || pass->Num > 100000) return;  // sanity: 69 rows + headroom
    // (Re)prime the baseline on: first read, a different saveSlot instance (world/save reload),
    // or a shrink (reset_days / new game). Primed entries are history, never re-broadcast.
    if (g_passBaseline < 0 || ss != g_polledSaveSlot ||
        !R::IsLiveByIndex(g_polledSaveSlot, g_polledSaveSlotIdx) || pass->Num < g_passBaseline) {
        g_polledSaveSlot = ss;
        g_polledSaveSlotIdx = ssIdx;
        g_passBaseline = pass->Num;
        UE_LOGI("event_fire: host poll primed (passEvents baseline=%d)", g_passBaseline);
        return;
    }
    if (pass->Num == g_passBaseline) return;
    // Growth: settime fired rows [baseline..Num). Broadcast each (scheduler fires are always
    // FireKind::RunEvent -- runSpecialEvent never appends here).
    if (!pass->Data) return;
    for (int32_t i = g_passBaseline; i < pass->Num; ++i) {
        const std::string name = NarrowName(pass->Data[i]);
        UE_LOGI("event_fire: host OBSERVED scheduler fire '%s' (passEvents %d -> %d)",
                name.c_str(), g_passBaseline, pass->Num);
        Broadcast(FireKind::RunEvent, name);
    }
    g_passBaseline = pass->Num;
}

void ClientSuppressTick() {
    void* ss = SaveSlotOf(Gamemode());
    if (!ss || g_offAllEvents < 0) return;
    RawArray* all = ArrayAt(ss, g_offAllEvents);
    if (!all || all->Num <= 0 || all->Num > 100000) return;  // 0 = already suppressed
    // A legal TArray state (Empty() with slack): Data/Max untouched, engine frees the same
    // allocation later. mainGamemode's boot ubergraph REBUILDS allEvents from the DataTable
    // every world load, so this re-asserts after any reload (and can never poison a save).
    g_zeroedAllEventsNum = all->Num;
    g_zeroedSaveSlot = ss;
    g_zeroedSaveSlotIdx = R::InternalIndexOf(ss);
    all->Num = 0;
    UE_LOGI("event_fire: client scheduler SUPPRESSED (allEvents %d -> 0; host is the only firer; "
            "restored on disconnect)", g_zeroedAllEventsNum);
}

void ClientDrainTick() {
    while (!g_pending.empty()) {
        if (!TryReplay(g_pending.front())) return;  // eventer not up yet -- keep the queue
        g_pending.pop_front();
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;
    ResolvePass();
    if (s->role() == coop::net::Role::Host) {
        HostPollTick();
    } else {
        ClientSuppressTick();
        ClientDrainTick();
    }
}

bool HostFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected() && s->role() != coop::net::Role::Host) {
        UE_LOGW("event_fire: HostFire refused -- connected as a client (host is authoritative)");
        return false;
    }
    const std::wstring ev = eventName;
    const std::wstring sp = specialName.empty() ? L"None" : specialName;
    GT::Post([kind, ev, sp] {
        // Resolve here, not before the Post: a SOLO host (dev menu, no session) never runs the
        // connected-gated Tick, so this task is its only resolution path. Game thread; cheap
        // once latched. NativeFire warns loudly if the world/eventer is not up.
        ResolvePass();
        if (!NativeFire(kind, ev, sp)) return;  // authority did not fire -> nothing to mirror
        // The dev seam: a direct runEvent never appends passEvents (bytecode-verified), so the
        // host poll cannot observe it -- broadcast at dispatch instead. Wire carries the name
        // only (never the special; receivers strip pranks by policy anyway).
        std::string narrow;
        narrow.reserve(ev.size());
        for (wchar_t c : ev) narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
        Broadcast(kind, narrow);
    });
    return true;
}

void OnReliable(const coop::net::EventFirePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: OnReliable off-game-thread -- dropping"); return; }
    ResolvePass();
    // NUL-bound the name (payload crosses the trust boundary; the dispatcher length-checked it).
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
        UE_LOGI("event_fire: '%s' NOT replayed -- %s owns the outputs", name.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: '%s' not in the replay policy -- default NO-replay (newer host? "
                "add the row's verdict)", name.c_str());
        return;
    }
    PendingFire pf{ kind, name };
    if (TryReplay(pf)) return;
    // One-shot rows dedupe at queue time too (a scheduler re-fire of a dev-fired row during the
    // same load window would otherwise queue twice; TryReplay would catch it later, but keeping
    // the queue duplicate-free keeps the cap honest).
    if (kind == FireKind::RunEvent) {
        for (const auto& q : g_pending)
            if (q.kind == kind && q.name == name) return;
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping '%s'",
                g_pending.size(), name.c_str());
        return;
    }
    UE_LOGI("event_fire: eventer not up yet -- queued '%s' (%zu pending)",
            name.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void ReplayInFlightRow(const std::string& rowName) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: ReplayInFlightRow off-game-thread -- dropping"); return; }
    if (rowName.empty()) return;
    ResolvePass();
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(rowName, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: in-flight '%s' NOT replayed -- %s owns the outputs (its join "
                "snapshot delivers current state)", rowName.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: in-flight '%s' not in the replay policy -- default NO-replay "
                "(add the row's verdict)", rowName.c_str());
        return;
    }
    PendingFire pf{ FireKind::RunEvent, rowName, /*activeOverride=*/true };
    if (TryReplay(pf)) return;
    // Eventer not up yet (world-ready races the actor resolve). If the row is already queued
    // (an EventFire copy landed in the pre-world window), UPGRADE it in place -- two entries
    // would double-dispatch, and the plain copy alone could history-skip the in-flight replay.
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
    UE_LOGI("event_fire: eventer not up yet -- queued in-flight '%s' (%zu pending)",
            rowName.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void OnDisconnect() {
    // Restore the client's scheduler: only if WE zeroed this exact live saveSlot and nothing
    // repopulated it since (boot rebuild leaves Num > 0 -- then the restore must not run).
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
    g_polledSaveSlot = nullptr;
    g_polledSaveSlotIdx = -1;
    g_passBaseline = -1;
    g_pending.clear();
    g_replayed.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::event_fire_sync
