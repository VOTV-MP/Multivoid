// coop/world/event_cue_sync.cpp -- see coop/world/event_cue_sync.h.
//
// HOST-AUTHORITATIVE: only the host detects and broadcasts (clients run a dormant scheduler --
// time_sync pins TimeScale=0 -- so they never fire an event). The detection is the eventer's own
// runEvent, watched through the script gate: the cue's emitter is spawned synchronously inside that
// body (an EX_CallMath SpawnEmitterAtLocation, invisible to our ProcessEvent detour), so the watch
// sees the fire the moment it happens, with its row name. Clients replay the emitter through a
// reflected SpawnEmitterAtLocation.

#include "coop/world/event_cue_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::event_cue_sync {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the cue registry: cosmetic emitter cues -----------------------------------------
// cueId == index. APPEND-ONLY -- the id is on the wire. starRain is bytecode-verified: the eventer's
// runEvent('starRain') spawns eff_shootingStar_rain at (0,0,6000) and returns (trigger_eventer @4709),
// and no other Blueprint spawns that template. A cue is detected by its runEvent row, so a new one
// needs its row and a location the body fixes; a cue whose position the body computes, or that spawns
// after a delay, needs the spawned component captured inside the body instead.
struct CueDef {
    const char* name;             // log label, and the list_events row that fires it
    const wchar_t* row;           // the runEvent row name
    const wchar_t* templateName;  // the UParticleSystem object the SpawnEmitter call uses
    ue_wrap::FVector loc;         // where the body spawns it
};
constexpr CueDef kCues[] = {
    { "starRain", L"starRain", L"eff_shootingStar_rain", { 0.f, 0.f, 6000.f } },  // Meteor Shower / Shooting Star
};
constexpr int kCueCount = static_cast<int>(sizeof(kCues) / sizeof(kCues[0]));

// Resolved on demand (a cue's particle asset loads with the eventer's class): the templates for the
// replay and the join snapshot, the rows' names for the watch. Game thread.
void* g_cueTemplates[kCueCount] = {};
R::FName g_cueRows[kCueCount] = {};
bool g_rowsResolved = false;

// Reflected spawn path (resolved once -- cloned from firefly_sync).
void* g_gameplayStaticsCdo = nullptr;
void* g_spawnEmitterFn     = nullptr;

std::atomic<unsigned> g_replays{0};  // emitters this peer replayed (ReplayCount)

// The host's watch on runEvent, registered once per process.
constexpr int kTagEventCue = 0x45564355;  // 'EVCU'
constexpr const wchar_t* kRunEvent = L"runEvent";  // one pointer: the gate matches a name watch by it
bool g_watchInstalled = false;
bool g_watchLive = false;
void* g_runEventFn = nullptr;     // trigger_eventer_C::runEvent, the one function of that name
int32_t g_offEventParam = -1;

void* CueTemplate(int cueId) {
    if (!g_cueTemplates[cueId])
        g_cueTemplates[cueId] = R::FindObject(kCues[cueId].templateName, L"ParticleSystem");
    return g_cueTemplates[cueId];
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

void Send(coop::net::Session& s, int slot, int cueId) {
    const ue_wrap::FVector& loc = kCues[cueId].loc;
    coop::net::EventCuePayload p{ static_cast<uint32_t>(cueId), loc.X, loc.Y, loc.Z };
    if (slot < 0) s.SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
    else s.SendReliableToSlot(slot, coop::net::ReliableKind::EventCue, &p, sizeof(p));
}

// HOST: the eventer's body has run for a row; if the row spawns a cue, send it. Every host fire
// counts, whoever called runEvent (the scheduler, the dev menu, the game's own event menu).
void OnRunEventPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolveRunEvent() || call.function != g_runEventFn) return;
    if (!g_rowsResolved) {
        for (int i = 0; i < kCueCount; ++i) g_cueRows[i] = ue_wrap::fname_utils::StringToFName(kCues[i].row);
        g_rowsResolved = true;
    }
    const R::FName& row = *reinterpret_cast<const R::FName*>(call.locals + g_offEventParam);
    for (int i = 0; i < kCueCount; ++i) {
        if (row.ComparisonIndex != g_cueRows[i].ComparisonIndex || row.Number != g_cueRows[i].Number) continue;
        Send(*s, -1, i);
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
        g_watchInstalled = sg::WatchName(kRunEvent, kTagEventCue, nullptr, &OnRunEventPost);
    if (g_watchInstalled && !g_watchLive) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRunEvent, kTagEventCue)) {
            g_watchLive = true;
            UE_LOGI("event_cue: the host's cue fires are seen at runEvent (a script-gate watch)");
        }
    }
}

void OnReliable(const coop::net::EventCuePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_cue: OnReliable off-game-thread -- dropping"); return; }
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
    for (int i = 0; i < kCueCount; ++i) anyLoaded |= (ctx.tmpls[i] = CueTemplate(i)) != nullptr;
    if (!anyLoaded) return;  // no cue asset loaded -> no live cue possible
    void* pscCls = ue_wrap::object_index::ClassByName(L"ParticleSystemComponent");
    if (!pscCls) return;
    ue_wrap::object_index::ForEachInstance(pscCls, [](void* c, void* psc, int32_t index) {
        auto& ctx = *static_cast<Ctx*>(c);
        if (!R::IsLiveByIndex(psc, index)) return;
        void* tmpl = ue_wrap::engine::GetParticleSystemTemplate(psc);
        for (int i = 0; i < kCueCount; ++i) {
            if (!tmpl || tmpl != ctx.tmpls[i]) continue;
            Send(*ctx.s, ctx.slot, i);
            UE_LOGI("event_cue: connect-snapshot -- re-sent live '%s' (cue %d) to slot %d",
                    kCues[i].name, i, ctx.slot);
        }
    }, &ctx);
    // The common case (no event running) stays silent -- the per-cue line above is the diagnostic.
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

unsigned ReplayCount() { return g_replays.load(std::memory_order_relaxed); }

}  // namespace coop::event_cue_sync
