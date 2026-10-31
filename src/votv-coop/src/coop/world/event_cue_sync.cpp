// coop/world/event_cue_sync.cpp -- see coop/world/event_cue_sync.h.
//
// HOST-AUTHORITATIVE: only the host detects and broadcasts (clients run a dormant scheduler --
// time_sync pins TimeScale=0 -- so they never fire an event). The detection is the eventer's own
// runEvent, watched through the script gate: the cue's emitter is spawned synchronously inside that
// body (an EX_CallMath SpawnEmitterAtLocation, invisible to our ProcessEvent detour), so the watch
// sees the fire the moment it happens, with its row name. Clients replay the emitter through a
// reflected SpawnEmitterAtLocation. A cue no row announces is taken at its emitter's own birth (the
// born mode, below).

#include "coop/world/event_cue_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/vent_crawler_sync.h"
#include "coop/world/world_sound_sync.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::event_cue_sync {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the cue registry: cosmetic emitter cues -----------------------------------------
// cueId == index. APPEND-ONLY -- the id is on the wire. A cue has one of two detections.
// ROW: the eventer's runEvent row spawns the emitter inside its body at a place the body fixes. starRain
//   is bytecode-verified: runEvent('starRain') spawns eff_shootingStar_rain at (0,0,6000) and returns
//   (trigger_eventer @4709), and no other Blueprint spawns that template.
// BORN: a cue whose body computes its place, or that no runEvent row announces. The host takes the
//   ParticleSystemComponent at its birth (the object index's create listener), reads its template at the
//   next tick, and sends the component's own world place. stolasHeal: growthArea_C.ReceiveTick spawns
//   eff_stolasHeal (SpawnEmitterAtLocation, no pooling) at the plant it fertilizes, and no other
//   Blueprint spawns that template.
struct CueDef {
    const char* name;             // log label, and the list_events row that fires it
    const wchar_t* row;           // ROW: the runEvent row name; BORN: nullptr
    const wchar_t* templateName;  // the UParticleSystem object the SpawnEmitter call uses
    const wchar_t* assetPath;     // that template's asset, which a client loads when the game has not (the replay needs it)
    ue_wrap::FVector loc;         // ROW: where the body spawns it; BORN: unused, the component's place is sent
    bool born;
};
constexpr CueDef kCues[] = {
    { "starRain", L"starRain", L"eff_shootingStar_rain", L"/Game/particles/eff_shootingStar_rain.eff_shootingStar_rain", { 0.f, 0.f, 6000.f }, false },  // Meteor Shower / Shooting Star
    { "stolasHeal", nullptr, L"eff_stolasHeal", L"/Game/particles/eff_stolasHeal.eff_stolasHeal", { 0.f, 0.f, 0.f }, true },  // the growth area's glow at the plant it fertilizes
};
constexpr int kCueCount = static_cast<int>(sizeof(kCues) / sizeof(kCues[0]));

// Resolved on demand (a cue's particle asset loads with the eventer's class): the templates for the
// replay and the join snapshot, the rows' names for the watch. Game thread. A template is held by a GC pin once it
// is found or loaded: the cue exists for the case that nothing else here references its emitter, and a raw pointer
// to an unreferenced asset dangles after the next collection.
ue_wrap::GcPin g_cueTemplates[kCueCount];
R::FName g_cueRows[kCueCount] = {};
bool g_rowsResolved = false;

// Reflected spawn path (resolved once -- cloned from firefly_sync).
void* g_gameplayStaticsCdo = nullptr;
void* g_spawnEmitterFn     = nullptr;

std::atomic<unsigned> g_replays{0};  // emitters this peer replayed (ReplayCount)
std::atomic<unsigned> g_sent{0};     // cues the host broadcast, to every peer or to one (SentCount)
float g_lastSent[3]   = {};          // the place of the latest of each, written on the game thread
float g_lastReplay[3] = {};

// The host's watch on runEvent, registered once per process.
constexpr int kTagEventCue = 0x45564355;  // 'EVCU'
UE_FACT_FN(kRowRunEvent, "event_cue_sync", nullptr, L"runEvent", AnyClass);
bool g_watchInstalled = false;
bool g_watchLive = false;
void* g_runEventFn = nullptr;     // trigger_eventer_C::runEvent, the one function of that name
int32_t g_offEventParam = -1;

// The born mode: the host's one object-index listener keeps each ParticleSystemComponent birth until the
// next tick, when its template (set after the birth) says whether it is a born cue's emitter.
struct Born { void* psc; int32_t index; };
std::vector<Born> g_born;                 // game thread: filled by the listener, emptied by DrainBorn
void* g_pscClass = nullptr;
bool g_listening = false;
bool g_listenRefused = false;
R::FName g_cueNames[kCueCount] = {};      // a born cue's template name, resolved once
bool g_cueNamesResolved = false;

// The template this call may use: the pinned one, else the object found now. A pin the engine's own root set refuses leaves
// the object to that owner and is used for this call only, never kept as a raw pointer.
void* CueTemplate(int cueId) {
    if (g_cueTemplates[cueId].Held()) return g_cueTemplates[cueId].Raw();
    void* found = R::FindObject(kCues[cueId].templateName, L"ParticleSystem");
    if (found) g_cueTemplates[cueId].Pin(found);
    return found;
}

// A client whose game has not loaded a cue's emitter (no class that spawns it has stood here yet) loads it by its asset
// path for the replay: the host's cue is not dropped for what the client has not met. A load that found nothing is not
// tried again; one that found it is pinned, so the next replay finds it in CueTemplate.
bool g_loadTried[kCueCount] = {};
void* LoadCueTemplate(int cueId) {
    if (g_loadTried[cueId]) return nullptr;
    void* loaded = ue_wrap::asset_load::LoadObjectByPath(kCues[cueId].assetPath);
    if (loaded) g_cueTemplates[cueId].Pin(loaded);
    else g_loadTried[cueId] = true;
    UE_LOGI("event_cue: cue %d ('%s') emitter was not loaded here; loading %ls by its path %s", cueId, kCues[cueId].name,
            kCues[cueId].assetPath, loaded ? "found it" : "did not find it");
    return loaded;
}

bool ResolveSpawn() {
    if (g_gameplayStaticsCdo && g_spawnEmitterFn) return true;
    if (!g_gameplayStaticsCdo)
        g_gameplayStaticsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gameplayStaticsCdo && !g_spawnEmitterFn) {
        if (void* cls = R::ClassOf(g_gameplayStaticsCdo))
            g_spawnEmitterFn = R::FindFunction(cls, L"SpawnEmitterAtLocation");
    }
    return g_gameplayStaticsCdo && g_spawnEmitterFn;
}

// The exact runEvent and its row parameter; the eventer's class loads with the world.
bool ResolveRunEvent() {
    if (g_offEventParam >= 0) return true;
    void* cls = ue_wrap::object_index::ClassByName(L"trigger_eventer_C");
    if (!cls) return false;
    g_runEventFn = R::FindFunction(cls, L"runEvent");
    g_offEventParam = g_runEventFn ? R::FindParamOffset(g_runEventFn, L"event") : -1;
    return g_offEventParam >= 0;
}

void Send(coop::net::Session& s, int slot, int cueId, const ue_wrap::FVector& loc) {
    coop::net::EventCuePayload p{ static_cast<uint32_t>(cueId), loc.X, loc.Y, loc.Z };
    if (slot < 0) s.SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
    else s.SendReliableToSlot(slot, coop::net::ReliableKind::EventCue, &p, sizeof(p));
    g_lastSent[0] = loc.X; g_lastSent[1] = loc.Y; g_lastSent[2] = loc.Z;
    g_sent.fetch_add(1, std::memory_order_relaxed);
}

// A born cue's template is matched by its name: the pointer lookup (CueTemplate) walks the object array
// on a miss, and every component born would ask. False until the string library resolves.
bool BornNamesReady() {
    if (g_cueNamesResolved) return true;
    if (!ue_wrap::fname_utils::Resolved()) return false;
    for (int i = 0; i < kCueCount; ++i)
        if (kCues[i].born) g_cueNames[i] = ue_wrap::fname_utils::StringToFName(kCues[i].templateName);
    g_cueNamesResolved = true;
    return true;
}

bool IsBornTemplate(void* tmpl, int cueId) {
    if (!tmpl || !kCues[cueId].born || !g_cueNamesResolved) return false;
    const R::FName& n = R::NameOf(tmpl);
    return n.ComparisonIndex == g_cueNames[cueId].ComparisonIndex && n.Number == g_cueNames[cueId].Number;
}

// The listener: a pointer compare of the class, and the birth kept. The object is not read here.
void OnBornCreated(void*, void* obj, void* cls, int32_t index) {
    if (cls != g_pscClass) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    g_born.push_back({ obj, index });
}

void EnsureBornListener() {
    if (g_listening || g_listenRefused) return;
    if (!g_pscClass) g_pscClass = ue_wrap::object_index::ClassByName(L"ParticleSystemComponent");
    if (!g_pscClass) return;
    g_listening = ue_wrap::object_index::AddListener(
        ue_wrap::object_index::Listener{ nullptr, &OnBornCreated, nullptr });
    if (!g_listening) {
        g_listenRefused = true;
        UE_LOGE("event_cue: the object index has no free listener slot -- born cues are not sent");
    }
}

// HOST, each pump tick: the components born since the last one, each a born cue's emitter or not. With no
// peer connected there is no one to send to, and a joiner meets the live ones in the connect scan.
void DrainBorn(coop::net::Session& s) {
    if (g_born.empty()) return;
    if (!s.connected()) { g_born.clear(); return; }
    if (!BornNamesReady()) return;
    for (const Born& b : g_born) {
        if (!R::IsLiveByIndex(b.psc, b.index)) continue;
        void* tmpl = ue_wrap::engine::GetParticleSystemTemplate(b.psc);
        for (int i = 0; i < kCueCount; ++i) {
            if (!IsBornTemplate(tmpl, i)) continue;
            ue_wrap::FVector at;
            if (!ue_wrap::engine::TryGetComponentLocation(b.psc, at)) {
                UE_LOGW("event_cue: '%s' (cue %d) born, its place did not read -- not sent", kCues[i].name, i);
                break;
            }
            Send(s, -1, i, at);
            UE_LOGI("event_cue: host broadcast '%s' (cue %d) born at (%.0f, %.0f, %.0f)", kCues[i].name, i,
                    at.X, at.Y, at.Z);
            break;
        }
    }
    g_born.clear();
}

// HOST: the eventer's body has run for a row; if the row spawns a cue, send it. Every host fire
// counts, whoever called runEvent (the scheduler, the dev menu, the game's own event menu).
void OnRunEventPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolveRunEvent() || call.function != g_runEventFn) return;
    if (!g_rowsResolved) {
        for (int i = 0; i < kCueCount; ++i)
            if (kCues[i].row) g_cueRows[i] = ue_wrap::fname_utils::StringToFName(kCues[i].row);
        g_rowsResolved = true;
    }
    const R::FName& row = *reinterpret_cast<const R::FName*>(call.locals + g_offEventParam);
    for (int i = 0; i < kCueCount; ++i) {
        if (!kCues[i].row) continue;
        if (row.ComparisonIndex != g_cueRows[i].ComparisonIndex || row.Number != g_cueRows[i].Number) continue;
        Send(*s, -1, i, kCues[i].loc);
        UE_LOGI("event_cue: host broadcast '%s' (cue %d) at (%.0f, %.0f, %.0f)", kCues[i].name, i,
                kCues[i].loc.X, kCues[i].loc.Y, kCues[i].loc.Z);
        return;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Called every pump tick by the install fanout, which is also the retry until the gate has
    // resolved the watch's name.
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRowRunEvent, kTagEventCue, nullptr, &OnRunEventPost);
    if (g_watchInstalled && !g_watchLive) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRowRunEvent, kTagEventCue)) {
            g_watchLive = true;
            UE_LOGI("event_cue: the host's cue fires are seen at runEvent (a script-gate watch)");
        }
    }
    // The born mode is the host's alone: a client never takes the listener.
    if (session && session->role() == coop::net::Role::Host) {
        EnsureBornListener();
        DrainBorn(*session);
    }
}

void OnReliable(const coop::net::EventCuePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_cue: OnReliable off-game-thread -- dropping"); return; }
    if (coop::vent_crawler_sync::OnCue(payload)) return;  // a crawl's start
    if (coop::world_sound_sync::OnCue(payload)) return;  // a sound cue, not an emitter
    if (payload.cueId >= static_cast<uint32_t>(kCueCount)) {
        UE_LOGW("event_cue: OnReliable unknown cueId=%u (have %d cues) -- dropping (newer host?)",
                payload.cueId, kCueCount);
        return;
    }
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z)) return;
    if (!ResolveSpawn()) {
        UE_LOGW("event_cue: OnReliable spawn path unresolved (cdo=%p fn=%p) -- dropping",
                g_gameplayStaticsCdo, g_spawnEmitterFn);
        return;
    }
    void* tmpl = CueTemplate(static_cast<int>(payload.cueId));
    if (!tmpl) tmpl = LoadCueTemplate(static_cast<int>(payload.cueId));
    if (!tmpl) {
        UE_LOGW("event_cue: cue %u ('%s') template not loaded on this peer -- dropping",
                payload.cueId, kCues[payload.cueId].name);
        return;
    }
    void* worldCtx = coop::players::Registry::Get().Local();
    if (!worldCtx) return;  // no local pawn yet -> can't resolve the world

    ue_wrap::FVector loc{ payload.x, payload.y, payload.z };
    ue_wrap::FVector scale{ 1.f, 1.f, 1.f };
    ue_wrap::FRotator rot{ 0.f, 0.f, 0.f };
    ue_wrap::ParamFrame f(g_spawnEmitterFn);
    f.Set<void*>(L"WorldContextObject", worldCtx);
    f.Set<void*>(L"EmitterTemplate", tmpl);
    f.SetRaw(L"Location", &loc, sizeof(loc));
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    f.SetRaw(L"Scale", &scale, sizeof(scale));
    f.Set<bool>(L"bAutoDestroy", true);
    f.Set<uint8_t>(L"PoolingMethod", 0);       // EPSCPoolMethod::None
    f.Set<bool>(L"bAutoActivateSystem", true);
    if (!ue_wrap::Call(g_gameplayStaticsCdo, f)) {
        UE_LOGW("event_cue: '%s' (cue %u) replay dispatch FAILED", kCues[payload.cueId].name, payload.cueId);
        return;
    }
    g_lastReplay[0] = payload.x; g_lastReplay[1] = payload.y; g_lastReplay[2] = payload.z;
    g_replays.fetch_add(1, std::memory_order_relaxed);
    UE_LOGI("event_cue: replayed '%s' (cue %u) at (%.0f, %.0f, %.0f)",
            kCues[payload.cueId].name, payload.cueId, payload.x, payload.y, payload.z);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    // HOST, at the joiner's world-ready. Each live cue emitter was sent when it spawned, and the
    // send gate dropped it for this still-loading slot; the gate opens in the same handler that
    // calls this, so every live one goes out here exactly once and every later one by its own send.
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    struct Ctx { coop::net::Session* s; int slot; void* tmpls[kCueCount]; };
    Ctx ctx{ s, peerSlot, {} };
    bool anyLoaded = false;
    for (int i = 0; i < kCueCount; ++i) {
        if (kCues[i].born) { anyLoaded |= BornNamesReady(); continue; }  // matched by name, no pointer to find
        anyLoaded |= (ctx.tmpls[i] = CueTemplate(i)) != nullptr;
    }
    if (!anyLoaded) return;  // no cue asset loaded -> no live cue possible
    void* pscCls = ue_wrap::object_index::ClassByName(L"ParticleSystemComponent");
    if (!pscCls) return;
    ue_wrap::object_index::ForEachInstance(pscCls, [](void* c, void* psc, int32_t index) {
        auto& ctx = *static_cast<Ctx*>(c);
        if (!R::IsLiveByIndex(psc, index)) return;
        void* tmpl = ue_wrap::engine::GetParticleSystemTemplate(psc);
        for (int i = 0; i < kCueCount; ++i) {
            if (!tmpl || (kCues[i].born ? !IsBornTemplate(tmpl, i) : tmpl != ctx.tmpls[i])) continue;
            ue_wrap::FVector at = kCues[i].loc;  // a born cue's place is its component's own
            if (kCues[i].born && !ue_wrap::engine::TryGetComponentLocation(psc, at)) {
                UE_LOGW("event_cue: connect-snapshot -- live '%s' (cue %d), its place did not read -- not sent",
                        kCues[i].name, i);
                continue;
            }
            Send(*ctx.s, ctx.slot, i, at);
            UE_LOGI("event_cue: connect-snapshot -- re-sent live '%s' (cue %d) at (%.0f, %.0f, %.0f) to slot %d",
                    kCues[i].name, i, at.X, at.Y, at.Z, ctx.slot);
        }
    }, &ctx);
    // The common case (no event running) stays silent -- the per-cue line above is the diagnostic.
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    g_born.clear();
}

unsigned ReplayCount() { return g_replays.load(std::memory_order_relaxed); }
unsigned SentCount() { return g_sent.load(std::memory_order_relaxed); }

void LastReplayAt(float (&xyz)[3]) { for (int i = 0; i < 3; ++i) xyz[i] = g_lastReplay[i]; }
void LastSentAt(float (&xyz)[3]) { for (int i = 0; i < 3; ++i) xyz[i] = g_lastSent[i]; }

}  // namespace coop::event_cue_sync
