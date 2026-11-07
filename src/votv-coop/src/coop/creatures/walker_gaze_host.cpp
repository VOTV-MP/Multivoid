// coop/creatures/walker_gaze_host.cpp -- see coop/creatures/walker_gaze_host.h.

#include "coop/creatures/walker_gaze_host.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/walker_graph.h"
#include "coop/creatures/walker_target.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::walker_gaze_host {
namespace {

namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace R = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace WG = coop::walker_graph;
namespace WT = coop::walker_target;

// The two natives the walker's tick judges its gaze with: the game's frustum test of the mesh's bounds, and the mesh's own
// WasRecentlyRendered (declared on a base of the skeletal mesh component, so the row names the component the walker holds).
UE_FACT_FN(kRowFrustum, "walker_gaze_host", L"bpCodeLib", L"isBoxInFrustum", Declared);
UE_FACT_FN(kRowRendered, "walker_gaze_host", L"SkeletalMeshComponent", L"WasRecentlyRendered", Dispatch);

// The host holds up to five walkers.
constexpr size_t kMaxWalkers = 8;

// One walker hunted by a client: its key (a message names it so), its target, and the level each slot last sent (a bit a slot).
// Only the target's bit is ever set, and a change of target clears them all.
struct Entry {
    void*              actor;
    int32_t            index;
    int32_t            serial;
    coop::net::WireKey key;
    uint8_t            target;
    uint32_t           seen;
};
Entry  g_entries[kMaxWalkers];
size_t g_count = 0;
bool   g_fullSaid = false;

// The hooks: installed once for the process, armed while an entry exists and the graph is pinned. `g_graph` is what each hook's
// one pointer compare reads.
std::atomic<void*> g_graph{nullptr};
void*              g_heldGraph = nullptr;    // the target module's word
void*              g_pinnedGraph = nullptr;  // the graph the sites were read from
WG::Calls          g_frustumSites;
WG::Calls          g_renderedSites;
void*              g_frustumFn = nullptr;
void*              g_renderedFn = nullptr;
bool               g_hooked = false;
bool               g_armed = false;
bool               g_subscribed = false;

// The gaze reads answered with a target's level, for the session: [dev], read by the walker drill (AnsweredReads).
std::atomic<uint32_t> g_answered{0};

// Why a word is refused: counted for the session and said once each.
enum Reason : int { kNotTarget, kUnknownKey, kTooFar, kBadFlag, kReasons };
const char* const kReasonText[kReasons] = {"the sender is not the walker's target", "no walker of that key is hunted by a client",
                                           "the sender's puppet is out of reach of the walker or cannot be placed",
                                           "the flag is not 0 or 1"};
uint32_t g_refused[kReasons];
bool     g_said[kReasons];

// The level changes the log has said: per slot, when it last did, how many came since (a line a second a slot at most) and the
// latest level with its walker, so the changes held back are written with the level they ended on.
struct LevelLog {
    uint64_t           lastMs = 0;
    uint32_t           held = 0;
    bool               seen = false;
    coop::net::WireKey key{};
};
constexpr uint64_t kLevelLogEveryMs = 1000;
LevelLog g_levelLog[coop::players::kMaxPeers];

// The changes held back for `slot`, written now with the level the slot's last change left. Called where the changes stop
// coming (a target change, a leaver, the session's end), so a burst never ends with its last level unsaid.
void FlushLevel(uint8_t slot) {
    LevelLog& log = g_levelLog[slot];
    if (log.held == 0) return;
    UE_LOGI("walker_gaze_host: slot %d %s walker '%ls' (%u change(s) since the last line not logged)", static_cast<int>(slot),
            log.seen ? "sees" : "no longer sees", coop::net::StringFromWireKey(log.key).c_str(), log.held);
    log.held = 0;
    log.lastMs = coop::net::NowMs();
}

void FlushAllLevels() {
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) FlushLevel(s);
}

bool In(const WG::Calls& calls, int32_t end) {
    const int n = calls.found < WG::kMaxCalls ? calls.found : WG::kMaxCalls;
    for (int i = 0; i < n; ++i)
        if (calls.ends[i] == end) return true;
    return false;
}

bool SameKey(const coop::net::WireKey& a, const coop::net::WireKey& b) {
    return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}

bool Alive(const Entry& e) {
    return R::SlotSerial(e.index) == e.serial && R::InternalIndexOf(e.actor) == e.index;
}

Entry* Find(void* walker) {
    for (size_t i = 0; i < g_count; ++i)
        if (g_entries[i].actor == walker) return &g_entries[i];
    return nullptr;
}

void Rearm();

void Remove(Entry* e) {
    *e = g_entries[--g_count];
    Rearm();
}

// Answer a gaze read of the walker `walker` with its target's level, if a client hunts it; any other read is left as the game
// made it. `result` is the native's return slot, a one-byte bool.
void Answer(void* walker) {
    Entry* e = Find(walker);
    void*  result = UH::CurrentResult();
    if (!e || !result) return;
    if (!Alive(*e)) {
        Remove(e);  // an entry that outlived its actor
        return;
    }
    *static_cast<uint8_t*>(result) = ((e->seen >> e->target) & 1u) ? 1 : 0;
    g_answered.fetch_add(1, std::memory_order_relaxed);
}

// The post hooks, game thread inside the engine's own calls. The first thing each does is one compare of the calling frame's
// function with the walker's graph, which every other caller of the native fails: per call of any other caller that is the
// frame read and the compare, and with no client hunting a walker the hooks are disarmed and the forwarder pays one load and a
// branch.
void OnFrustumPost(void* /*context*/, void* source, void* /*result*/) {
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    void* const           graph = g_graph.load(std::memory_order_relaxed);
    if (frame.function != graph || !graph) return;
    if (!GT::IsGameThread() || !In(g_frustumSites, frame.codeOffset)) return;
    Answer(source);
}

void OnRenderedPost(void* /*context*/, void* source, void* /*result*/) {
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    void* const           graph = g_graph.load(std::memory_order_relaxed);
    if (frame.function != graph || !graph) return;
    if (!GT::IsGameThread() || !In(g_renderedSites, frame.codeOffset)) return;
    Answer(source);
}

void Disarm() {
    g_graph.store(nullptr, std::memory_order_relaxed);
    if (!g_armed) return;
    UH::SetArmed(g_frustumFn, &OnFrustumPost, false);
    UH::SetArmed(g_renderedFn, &OnRenderedPost, false);
    g_armed = false;
}

// Armed while a client hunts a held walker and the graph is pinned.
void Rearm() {
    // The mannequin drill's red: the host never answers the walker's gaze reads with its target's, so they stay the host's own
    // and the drill's stop step must FAIL.
    static const bool s_redNoRoute = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill) == "no_gaze_route";
    if (s_redNoRoute || g_count == 0 || !g_heldGraph || g_heldGraph != g_pinnedGraph || !g_hooked) {
        Disarm();
        return;
    }
    g_graph.store(g_heldGraph, std::memory_order_relaxed);
    if (g_armed) return;
    g_armed = UH::SetArmed(g_frustumFn, &OnFrustumPost, true) && UH::SetArmed(g_renderedFn, &OnRenderedPost, true);
}

// The graph's calls of both natives, pinned once per graph, and the hooks installed once for the process. A graph that does not
// give both leaves the walker's gaze reads to the game, so the walker freezes on the host's camera as in single player.
void Pin(void* graph) {
    if (graph == g_pinnedGraph) return;
    g_pinnedGraph = nullptr;
    const ue_wrap::facts::Resolved frustum = ue_wrap::facts::Resolve(kRowFrustum);
    const ue_wrap::facts::Resolved rendered = ue_wrap::facts::Resolve(kRowRendered);
    if (!frustum || !rendered || !WG::FindCalls(graph, frustum.Fn(), g_frustumSites) ||
        !WG::FindCalls(graph, rendered.Fn(), g_renderedSites)) {
        UE_LOGE("walker_gaze_host: the walker's graph or a gaze native did not resolve -- a walker freezes on the host's camera");
        return;
    }
    if (g_frustumSites.found < 1 || g_frustumSites.found > WG::kMaxCalls || g_renderedSites.found < 1 ||
        g_renderedSites.found > WG::kMaxCalls) {
        UE_LOGE("walker_gaze_host: %d frustum call(s) and %d rendered call(s) in the walker's graph (1 to %d of each are "
                "expected) -- a walker freezes on the host's camera", g_frustumSites.found, g_renderedSites.found, WG::kMaxCalls);
        return;
    }
    if (!g_hooked) {
        if (!UH::InstallPostHook(frustum, &OnFrustumPost, /*armed=*/false) ||
            !UH::InstallPostHook(rendered, &OnRenderedPost, /*armed=*/false)) {
            UE_LOGE("walker_gaze_host: a gaze seam did not install -- a walker freezes on the host's camera");
            return;
        }
        g_frustumFn = frustum.Fn();
        g_renderedFn = rendered.Fn();
        g_hooked = true;
    }
    g_pinnedGraph = graph;
    UE_LOGI("walker_gaze_host: the walker reads its gaze at %d frustum site(s), the first ending at offset %d, and %d rendered "
            "site(s), the first ending at %d", g_frustumSites.found, g_frustumSites.ends[0], g_renderedSites.found,
            g_renderedSites.ends[0]);
}

// The target module's word: the walker graph while a walker is held, null when none is.
void OnGraph(void* graph) {
    g_heldGraph = graph;
    if (graph) Pin(graph);
    Rearm();
}

Entry* Make(void* walker) {
    for (size_t i = 0; i < g_count;) {
        if (Alive(g_entries[i])) ++i;
        else g_entries[i] = g_entries[--g_count];
    }
    if (g_count >= kMaxWalkers) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("walker_gaze_host: %zu walkers are hunted by clients -- another is judged by the host's camera (said once)",
                    kMaxWalkers);
        }
        return nullptr;
    }
    const std::wstring key = ue_wrap::prop::GetKeyString(walker);
    if (key.empty()) {
        UE_LOGW("walker_gaze_host: walker %p has no key -- a client's gaze cannot be named for it", walker);
        return nullptr;
    }
    Entry& e = g_entries[g_count++];
    e = Entry{walker, R::InternalIndexOf(walker), 0, {}, WT::kNoTarget, 0};
    e.serial = R::SlotSerial(e.index);
    coop::net::WireKeyFromString(key, e.key);
    return &e;
}

// The hunt changed or ended. A client's target makes (or keeps) the walker's entry with every level cleared; any other target,
// the host's or none, and a walker that ended, drop it. `walker` may be dying: it is compared and never read through here.
void OnTargetChanged(void* walker, uint8_t slot) {
    FlushAllLevels();
    Entry* e = Find(walker);
    // An entry is the walker of its index and serial, not of its address: one whose actor ended and whose address a new walker
    // took is dropped, so the new walker is made afresh with its own key, index and serial.
    if (e && !Alive(*e)) {
        Remove(e);
        e = nullptr;
    }
    const bool client = slot != WT::kNoTarget && slot != 0 && slot < coop::players::kMaxPeers;
    if (!client) {
        if (e) Remove(e);
        return;
    }
    if (!e) e = Make(walker);
    if (!e) return;
    e->target = slot;
    e->seen = 0;
    UE_LOGI("walker_gaze_host: walker %p is hunted by client slot %d -- its gaze reads answer with that slot's level", walker,
            static_cast<int>(slot));
    Rearm();
}

// An actor's place from its root in memory where the root is unattached, through the engine otherwise (as explosion_parking
// reads a prop's).
bool Locate(void* actor, ue_wrap::FVector& out) {
    return E::TryGetRootLocationRaw(actor, out) || E::TryGetActorLocation(actor, out);
}

// A change of a slot's level, logged at most once a second per slot with the changes held back since (a target that toggles as
// fast as the lane carries costs the host one counter bump per word, not one log write).
void LogLevel(uint8_t slot, bool seen, const coop::net::WireKey& key) {
    LevelLog&      log = g_levelLog[slot];
    // Held changes belong to the walker whose key they were held under: a change of walker writes them first.
    if (log.held > 0 && !SameKey(log.key, key)) FlushLevel(slot);
    log.seen = seen;
    log.key = key;
    const uint64_t now = coop::net::NowMs();
    if (now - log.lastMs < kLevelLogEveryMs) {
        ++log.held;
        return;
    }
    log.lastMs = now;
    if (log.held > 0)
        UE_LOGI("walker_gaze_host: slot %d %s walker '%ls' (%u more change(s) in the last second not logged)", static_cast<int>(slot),
                seen ? "sees" : "no longer sees", coop::net::StringFromWireKey(key).c_str(), log.held);
    else
        UE_LOGI("walker_gaze_host: slot %d %s walker '%ls'", static_cast<int>(slot), seen ? "sees" : "no longer sees",
                coop::net::StringFromWireKey(key).c_str());
    log.held = 0;
}

void Refuse(Reason why, uint8_t slot) {
    ++g_refused[why];
    if (g_said[why]) return;
    g_said[why] = true;
    // A target's last word can cross the host's change of target on the wire: that is ordinary, so it is counted and said once
    // as information, where every other reason is a warning.
    if (why == kNotTarget) {
        UE_LOGI("walker_gaze_host: a WalkerSeen from slot %d is refused: %s (counted, said once; a former target's word in "
                "flight is ordinary)", static_cast<int>(slot), kReasonText[why]);
        return;
    }
    UE_LOGW("walker_gaze_host: a WalkerSeen from slot %d is refused: %s (counted, said once)", static_cast<int>(slot),
            kReasonText[why]);
}

}  // namespace

void Install(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("walker_gaze_host::Install");
    if (session.role() != coop::net::Role::Host || g_subscribed) return;
    g_subscribed = true;
    const bool target = WT::SubscribeTargetChanged(&OnTargetChanged);
    const bool graph = WT::SubscribeGraph(&OnGraph);
    if (!target || !graph)
        UE_LOGE("walker_gaze_host: the target module's subscriber table is full (target %d, graph %d) -- a client's gaze does not "
                "freeze its walker", target ? 1 : 0, graph ? 1 : 0);
}

void OnSeen(const coop::net::WalkerSeenPayload& p, uint8_t senderSlot) {
    if (p.seen > 1) return Refuse(kBadFlag, senderSlot);
    Entry* e = nullptr;
    for (size_t i = 0; i < g_count; ++i)
        if (SameKey(g_entries[i].key, p.key)) {
            e = &g_entries[i];
            break;
        }
    if (!e || !Alive(*e)) return Refuse(kUnknownKey, senderSlot);
    if (e->target != senderSlot) return Refuse(kNotTarget, senderSlot);
    // The walker was found by its key in this module's own table (fed by the target module's changes), so no lookup is reflected.
    // A word that repeats the level held costs this one compare: the reach is read, from the sender's body and the walker, only
    // for a word that changes the level, one read of the sender's distance each.
    const uint32_t bit = 1u << senderSlot;
    const bool     was = (e->seen & bit) != 0;
    if (was == (p.seen != 0)) return;
    ue_wrap::FVector from{}, to{};
    void* const      body = WT::ClientBody(senderSlot);
    if (!body || !Locate(body, from) || !Locate(e->actor, to)) return Refuse(kTooFar, senderSlot);
    const float dx = from.X - to.X, dy = from.Y - to.Y, dz = from.Z - to.Z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kWalkerGazeReachCm) return Refuse(kTooFar, senderSlot);
    e->seen = p.seen ? (e->seen | bit) : (e->seen & ~bit);
    LogLevel(senderSlot, p.seen != 0, p.key);
}

uint32_t AnsweredReads() { return g_answered.load(std::memory_order_relaxed); }

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return;
    FlushLevel(slot);
    for (size_t i = 0; i < g_count; ++i) g_entries[i].seen &= ~(1u << slot);
}

void OnDisconnect() {
    for (int i = 0; i < kReasons; ++i)
        if (g_refused[i] > 0) UE_LOGI("walker_gaze_host: %u WalkerSeen refused over the session: %s", g_refused[i], kReasonText[i]);
    for (int i = 0; i < kReasons; ++i) {
        g_refused[i] = 0;
        g_said[i] = false;
    }
    FlushAllLevels();
    for (LevelLog& l : g_levelLog) l = LevelLog{};
    g_answered.store(0, std::memory_order_relaxed);
    g_count = 0;
    g_fullSaid = false;
    g_heldGraph = nullptr;
    Disarm();
}

}  // namespace coop::walker_gaze_host
