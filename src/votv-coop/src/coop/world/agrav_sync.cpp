// coop/world/agrav_sync.cpp -- see coop/world/agrav_sync.h.
//
// The bytecode this stands on (trigger_agrav_C): gaher sphere-overlaps prop_C within 100 m of the camera and appends
// each root primitive component to `comps` (never cleared, so entries of an earlier run can be dead); runTrigger
// shuffles a copy, and every `time` seconds (the event's duration over the count) sets one component's gravity off
// with a (0,0,10) velocity and a spin of 1 to 5 deg/s; after a 5 to 7 s hold it sets gravity back on, one per frame;
// setEvent(false) ends it. So a component of `comps` with gravity off is a prop the event has lifted and not yet put
// back, and a run lifts every prop of `comps` at once for most of its length.
//
// What the drive channel carries: one queue of 16 rows per send and one send per net tick (60 Hz), so at most 960
// rows a second whatever the number of props (80 B a row, 77 KB/s a client), shared in turn by every driven prop,
// the hook lane's and a thrown prop's among them. A lifted prop sent about 8 rows a second in the two-peer run, so
// about 120 props fill the queue and each gets its natural rate. A drive row cannot be closed from outside while
// its prop moves, so a slot is held until the prop's gravity returns.

#include "coop/world/agrav_sync.h"

#include "coop/element/element.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/world/active_events.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::agrav_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace AE = ue_wrap::active_events;
namespace P  = ue_wrap::profile;
using Clock = std::chrono::steady_clock;

// Props held on the drive at once: the channel's rows per second over a lifted prop's natural rate (above).
constexpr size_t kMaxHeld = 128;
constexpr auto   kPollSpacing = std::chrono::milliseconds(200);
constexpr size_t kScanSlice = 160;      // components whose gravity a poll reads besides the held ones
constexpr size_t kClaimsPerPoll = 32;
constexpr auto   kCutSpacing = std::chrono::seconds(1);
constexpr double kCorrectPerSecond = 40.0;  // end corrections handed to the drive per second
constexpr size_t kCorrectBurst = 8;
constexpr float  kSettledCm = 1.5f;     // a lifted prop that ended within this of where it was seen needs no correction
constexpr float  kSettledDeg = 1.5f;
constexpr int    kMaxRefusals = 3;

// A prop the event has lifted: seen with gravity off, and where it stood then (where the clients' copy stands).
struct Rec {
    void*             comp;
    void*             actor;
    int32_t           actorIdx;
    ue_wrap::FVector  loc0;
    ue_wrap::FRotator rot0;
    bool              pose0Ok;
    bool              held = false;
    bool              streamed = false;   // held at some point while lifted
    bool              restored = false;   // its gravity came back
    int               refused = 0;
};
std::vector<Rec>                  g_recs;
std::unordered_map<void*, size_t> g_recOf;   // component -> index in g_recs
size_t   g_heldCount = 0;
size_t   g_cursor = 0;                       // where the next poll's slice of `comps` starts
bool     g_active = false;                   // a trigger_agrav_C was in flight at the last poll
bool     g_fullSaid = false;
float    g_cutD2 = FLT_MAX;                  // the squared distance of the kMaxHeld-th nearest prop of `comps`
Clock::time_point g_nextPoll{};
Clock::time_point g_nextCut{};
std::vector<ue_wrap::FVector> g_players;     // the live players' places at the last poll

// The end of an event: the lifted props that were never streamed, handed to the drive in turn, nearest first.
struct Correction {
    void*   actor;
    int32_t actorIdx;
};
std::vector<Correction> g_queue;
size_t   g_queueHead = 0;
double   g_allowance = 0.0;
Clock::time_point g_lastCorrect{};
struct Report {
    size_t lifted = 0, streamed = 0, corrected = 0, neverSent = 0, settled = 0;
};
Report   g_report;
bool     g_reportOpen = false;

// The trigger's `comps` member, resolved on the class and again when the class is a new object.
void*   g_triggerCls = nullptr;
int32_t g_offComps = -1;

struct RawPtrArray {
    void**  data;
    int32_t num;
    int32_t max;
};

// UPrimitiveComponent::IsGravityEnabled(), found up the class chain from the native primitive class.
constexpr int32_t kFrameBytes = 64;
void*   g_gravityFn = nullptr;
int32_t g_gravityRet = -1;
bool    g_gravityMissed = false;  // said once; the read answers unknown from then on
ue_wrap::CachedObjRef g_primitiveCls;

bool ResolveGravityFn() {
    if (g_gravityMissed) return false;
    if (!g_primitiveCls.Alive()) {
        g_primitiveCls.Set(R::FindClass(P::name::PrimitiveComponentClass));
        g_gravityFn = nullptr;
    }
    if (g_gravityFn) return true;
    void* cls = g_primitiveCls.Raw();
    if (!cls) return false;
    g_gravityFn = R::FindDispatchFunction(cls, L"IsGravityEnabled", nullptr);
    g_gravityRet = g_gravityFn ? R::FindParamOffset(g_gravityFn, L"ReturnValue") : -1;
    if (!g_gravityFn || g_gravityRet < 0 || R::FunctionFrameSize(g_gravityFn) > kFrameBytes) {
        UE_LOGW("agrav_sync: IsGravityEnabled did not resolve (fn=%p ReturnValue@%d) -- the event's props are not "
                "streamed", g_gravityFn, g_gravityRet);
        g_gravityFn = nullptr;
        g_gravityMissed = true;
        return false;
    }
    return true;
}

// 1 gravity on, 0 off, -1 unknown (not a live primitive component, or the call did not run).
int GravityOf(void* comp) {
    if (!E::IsPrimitiveComponent(comp) || !ResolveGravityFn()) return -1;
    unsigned char frame[kFrameBytes] = {};
    if (!R::CallFunction(comp, g_gravityFn, frame)) return -1;
    return frame[g_gravityRet] ? 1 : 0;
}

// The live trigger_agrav_C in the game's registry of running events, or null.
void* FindTrigger() {
    void* cls = ue_wrap::object_index::ClassByName(L"trigger_agrav_C");
    if (!cls || !AE::EnsureResolved()) return nullptr;
    AE::Senders senders{};
    if (!AE::ReadSenders(senders) || senders.num < 0 || senders.num > 4096) return nullptr;
    for (int32_t i = 0; i < senders.num; ++i) {
        void* obj = senders.data ? senders.data[i] : nullptr;
        if (!obj || !R::IsLive(obj) || R::ClassOf(obj) != cls) continue;
        if (cls != g_triggerCls) {
            g_triggerCls = cls;
            g_offComps = R::FindPropertyOffset(cls, L"comps");
            if (g_offComps < 0) UE_LOGW("agrav_sync: trigger_agrav_C.comps did not resolve -- the event's props are not streamed");
        }
        return obj;
    }
    return nullptr;
}

bool PoseOf(void* actor, ue_wrap::FVector& loc, ue_wrap::FRotator& rot) {
    return E::TryGetRootPoseRaw(actor, loc, rot) || (E::TryGetActorLocation(actor, loc) && E::TryGetActorRotation(actor, rot));
}

bool LocationOf(void* actor, ue_wrap::FVector& loc) {
    return E::TryGetRootLocationRaw(actor, loc) || E::TryGetActorLocation(actor, loc);
}

// The players' places, the host's and each client's puppet's.
void ReadPlayers() {
    g_players.clear();
    void* bodies[8] = {};
    const int n = coop::players::Registry::Get().LiveBodies(bodies, 8);
    for (int i = 0; i < n; ++i) {
        ue_wrap::FVector p{};
        if (LocationOf(bodies[i], p)) g_players.push_back(p);
    }
}

// Squared distance from `p` to the nearest player; 0 when no player's place could be read, so no prop is far.
float NearestD2(const ue_wrap::FVector& p) {
    float best = FLT_MAX;
    for (const ue_wrap::FVector& q : g_players) {
        const float dx = p.X - q.X, dy = p.Y - q.Y, dz = p.Z - q.Z;
        best = (std::min)(best, dx * dx + dy * dy + dz * dz);
    }
    return g_players.empty() ? 0.f : best;
}

void LetGo(const Rec& r) {
    if (R::IsLiveByIndex(r.actor, r.actorIdx)) coop::prop_drive_host::Release(r.actor);
}

// Props whose gravity is back (or that are gone) are let go to coast to rest.
void LetGoRestored() {
    for (Rec& r : g_recs) {
        if (!r.held) continue;
        if (R::IsLiveByIndex(r.actor, r.actorIdx) && GravityOf(r.comp) == 0) continue;
        LetGo(r);
        r.held = false;
        r.restored = true;
        --g_heldCount;
    }
}

// A slice of `comps` each poll: a component seen with gravity off becomes a record; a recorded one whose gravity
// returned is marked, so it is not asked for a slot.
void ScanSlice(const RawPtrArray& comps) {
    const size_t num = static_cast<size_t>(comps.num);
    const size_t take = (std::min)(kScanSlice, num);
    for (size_t n = 0; n < take; ++n) {
        void* comp = comps.data[(g_cursor + n) % num];
        if (!comp) continue;
        const auto it = g_recOf.find(comp);
        if (it != g_recOf.end()) {
            Rec& r = g_recs[it->second];
            if (!r.held && !r.restored && GravityOf(comp) == 1) r.restored = true;
            continue;
        }
        if (GravityOf(comp) != 0) continue;
        void* actor = R::OuterOf(comp);
        if (!actor || !R::IsLive(actor)) continue;
        Rec r{};
        r.comp = comp;
        r.actor = actor;
        r.actorIdx = R::InternalIndexOf(actor);
        r.pose0Ok = PoseOf(actor, r.loc0, r.rot0);
        g_recOf.emplace(comp, g_recs.size());
        g_recs.push_back(r);
    }
    g_cursor = (g_cursor + take) % num;
}

// The squared distance of the kMaxHeld-th nearest prop of `comps` to any player: a lifted prop beyond it is not
// among the nearest the slots are for. Every prop of `comps` is lifted before the event ends, so ranking all of
// them, not only the ones lifted so far, keeps the slots from going to whichever prop the shuffle lifts first.
void UpdateCut(const RawPtrArray& comps) {
    g_cutD2 = FLT_MAX;
    if (g_players.empty()) return;
    std::vector<float> d2;
    d2.reserve(static_cast<size_t>(comps.num));
    for (int32_t i = 0; i < comps.num; ++i) {
        void* actor = (comps.data[i] && R::IsLive(comps.data[i])) ? R::OuterOf(comps.data[i]) : nullptr;
        ue_wrap::FVector p{};
        if (!actor || !R::IsLive(actor) || !LocationOf(actor, p)) continue;
        d2.push_back(NearestD2(p));
    }
    if (d2.size() <= kMaxHeld) return;
    std::nth_element(d2.begin(), d2.begin() + static_cast<std::ptrdiff_t>(kMaxHeld - 1), d2.end());
    g_cutD2 = d2[kMaxHeld - 1];
}

// Free slots go to the nearest lifted props inside the cut, up to kClaimsPerPoll a poll.
void Admit() {
    std::vector<std::pair<float, size_t>> want;
    for (size_t i = 0; i < g_recs.size(); ++i) {
        const Rec& r = g_recs[i];
        if (r.held || r.streamed || r.restored || r.refused >= kMaxRefusals) continue;
        ue_wrap::FVector p{};
        if (!R::IsLiveByIndex(r.actor, r.actorIdx) || !LocationOf(r.actor, p)) continue;
        const float d2 = NearestD2(p);
        if (d2 <= g_cutD2) want.emplace_back(d2, i);
    }
    if (want.empty()) return;
    if (g_heldCount >= kMaxHeld && !g_fullSaid) {
        g_fullSaid = true;
        UE_LOGI("agrav_sync: all %zu slots in use -- the farther lifted props float on the host only until the "
                "event ends", kMaxHeld);
    }
    std::sort(want.begin(), want.end());
    size_t claims = 0;
    for (const auto& w : want) {
        if (g_heldCount >= kMaxHeld || claims >= kClaimsPerPoll) break;
        Rec& r = g_recs[w.second];
        if (GravityOf(r.comp) != 0) {
            r.restored = true;
            continue;
        }
        const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(r.actor);
        if (eid == coop::element::kInvalidId || eid == 0u) continue;  // not a prop the host has named (yet)
        coop::prop_drive_host::Claim(r.actor, "agrav");
        ++claims;
        if (!coop::prop_drive_host::ContextOf(r.actor, static_cast<uint32_t>(eid)).open) {
            ++r.refused;   // the drive refused it: a hand holds it, or its class is not on the wire
            continue;
        }
        r.held = r.streamed = true;
        ++g_heldCount;
    }
}

void PrintReport() {
    UE_LOGI("agrav_sync: event over -- %zu lifted, %zu streamed while lifted, %zu corrected at the end, %zu never sent "
            "(%zu more ended where they were seen)", g_report.lifted, g_report.streamed, g_report.corrected,
            g_report.neverSent, g_report.settled);
    g_report = Report{};
    g_reportOpen = false;
}

// The event is over: held props are let go, and every lifted prop that was never streamed and moved is queued for
// the drive, nearest to a player first. A streamed prop ended by the drive's own rest rule at its final pose.
void EndEvent() {
    if (!g_active) return;
    for (const Rec& r : g_recs)
        if (r.held) LetGo(r);
    ReadPlayers();
    std::vector<std::pair<float, Correction>> due;
    for (const Rec& r : g_recs) {
        ++g_report.lifted;
        if (r.streamed) {
            ++g_report.streamed;
            continue;
        }
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!R::IsLiveByIndex(r.actor, r.actorIdx) || !PoseOf(r.actor, loc, rot)) {
            ++g_report.neverSent;
            continue;
        }
        if (r.pose0Ok && std::fabs(loc.X - r.loc0.X) <= kSettledCm && std::fabs(loc.Y - r.loc0.Y) <= kSettledCm &&
            std::fabs(loc.Z - r.loc0.Z) <= kSettledCm &&
            std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - r.rot0.Pitch)) <= kSettledDeg &&
            std::fabs(ue_wrap::NormalizeAxis(rot.Yaw - r.rot0.Yaw)) <= kSettledDeg &&
            std::fabs(ue_wrap::NormalizeAxis(rot.Roll - r.rot0.Roll)) <= kSettledDeg) {
            ++g_report.settled;
            continue;
        }
        due.push_back({NearestD2(loc), Correction{r.actor, r.actorIdx}});
    }
    std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (g_queueHead >= g_queue.size()) {
        g_queue.clear();
        g_queueHead = 0;
    }
    for (const auto& d : due) g_queue.push_back(d.second);
    g_reportOpen = true;
    g_allowance = static_cast<double>(kCorrectBurst);
    g_lastCorrect = Clock::now();
    g_recs.clear();
    g_recOf.clear();
    g_heldCount = 0;
    g_cursor = 0;
    g_fullSaid = false;
    g_cutD2 = FLT_MAX;
    g_active = false;
    UE_LOGI("agrav_sync: trigger gone -- %zu prop(s) queued for the end correction, %zu per second",
            g_queue.size() - g_queueHead, static_cast<size_t>(kCorrectPerSecond));
}

// Each tick, the end corrections due: a prop handed to the drive is read once at the host's pose, sent as a row and
// ended with the drive's End edge at rest, which sets the client's copy to that pose and gives it back its physics.
void RunCorrections() {
    if (g_queueHead >= g_queue.size()) {
        if (g_reportOpen) PrintReport();
        return;
    }
    const auto now = Clock::now();
    g_allowance = (std::min)(static_cast<double>(kCorrectBurst),
                             g_allowance + std::chrono::duration<double>(now - g_lastCorrect).count() * kCorrectPerSecond);
    g_lastCorrect = now;
    while (g_allowance >= 1.0 && g_queueHead < g_queue.size()) {
        const Correction c = g_queue[g_queueHead++];
        g_allowance -= 1.0;
        const coop::element::ElementId eid = R::IsLiveByIndex(c.actor, c.actorIdx)
                                                 ? coop::prop_element_tracker::GetPropElementIdForActor(c.actor)
                                                 : coop::element::kInvalidId;
        if (eid == coop::element::kInvalidId || eid == 0u) {
            ++g_report.neverSent;
            continue;
        }
        coop::prop_drive_host::Coast(c.actor, "agrav end");
        if (coop::prop_drive_host::ContextOf(c.actor, static_cast<uint32_t>(eid)).open) ++g_report.corrected;
        else                                                                              ++g_report.neverSent;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    UE_ASSERT_GAME_THREAD("agrav_sync::Tick");
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    RunCorrections();
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollSpacing;

    void* trigger = FindTrigger();
    if (!trigger) {
        EndEvent();
        return;
    }
    if (!g_active) {
        g_active = true;
        UE_LOGI("agrav_sync: trigger_agrav_C in flight -- the nearest %zu props it lifts are streamed to the clients, "
                "the rest corrected at its end", kMaxHeld);
    }
    if (g_offComps < 0) return;
    const auto* comps = reinterpret_cast<const RawPtrArray*>(static_cast<const uint8_t*>(trigger) + g_offComps);
    if (!comps->data || comps->num <= 0 || comps->num > 100000) return;
    LetGoRestored();
    ScanSlice(*comps);
    ReadPlayers();
    if (now >= g_nextCut) {
        g_nextCut = now + kCutSpacing;
        UpdateCut(*comps);
    }
    Admit();
}

void OnDisconnect() {
    g_recs.clear();
    g_recOf.clear();
    g_queue.clear();
    g_queueHead = 0;
    g_heldCount = 0;
    g_cursor = 0;
    g_active = false;
    g_fullSaid = false;
    g_reportOpen = false;
    g_report = Report{};
    g_cutD2 = FLT_MAX;
    g_nextPoll = {};
    g_nextCut = {};
}

}  // namespace coop::agrav_sync
