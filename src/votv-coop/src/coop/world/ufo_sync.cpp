// coop/world/ufo_sync.cpp -- see coop/world/ufo_sync.h.
//
// The UFOs' graphs, from their bytecode (0.9.0n). ufo_ballfo_C: set(), run by its construction script and its begin
// play, turns `center` to a random yaw and puts its ends `a` and `b` at random; the tick carries `balls` from where it
// is to `b` and destroys the actor there (its ReceiveDestroyed ends the event). ufo_boofo_C (ufo_boofo_spawn1_C is a
// child with its own points): begin play builds `travelSpline` from its `points`, then gen() fills `flySpline` from
// it, each point turned a random angle round the path; the tick, switched on after 5 s, flies `ufo` along flySpline
// and ends the event and destroys the actor at the end. ufo_pillfo_C and ufo_joel_C fly timelines with no roll and
// end the event themselves before their destroy. midasufotest_C: runTrigger, while not active, sets it active, and
// its tick flies its spline once.

#include "coop/world/ufo_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/creature_event_relay.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace coop::ufo_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace N  = coop::net;

UE_FACT_FN(kRowMidasRunTrigger, "ufo_sync", L"midasufotest_C", L"runTrigger", Declared);
constexpr int kTagMidas = 0x55464F01;  // 'UFO' 1

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;

bool IsHost() { return g_session && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->role() == N::Role::Client; }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

enum class Kind : uint8_t { None, Ball, Boo, Pill, Joel };

Kind KindOf(void* actor) {
    if (!actor) return Kind::None;
    const std::wstring n = R::ClassNameOf(actor);
    if (_wcsicmp(n.c_str(), L"ufo_ballfo_C") == 0) return Kind::Ball;
    if (_wcsicmp(n.c_str(), L"ufo_boofo_C") == 0 || _wcsicmp(n.c_str(), L"ufo_boofo_spawn1_C") == 0) return Kind::Boo;
    if (_wcsicmp(n.c_str(), L"ufo_pillfo_C") == 0) return Kind::Pill;
    if (_wcsicmp(n.c_str(), L"ufo_joel_C") == 0) return Kind::Joel;
    return Kind::None;
}

// A component member of `actor`, by its property name.
void* Component(void* actor, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    return off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(actor) + off) : nullptr;
}

ue_wrap::FVector RelLoc(void* comp) {
    return *reinterpret_cast<const ue_wrap::FVector*>(static_cast<const char*>(comp) +
                                                      ue_wrap::profile::off::USceneComponent_RelativeLocation);
}
ue_wrap::FRotator RelRot(void* comp) {
    return *reinterpret_cast<const ue_wrap::FRotator*>(static_cast<const char*>(comp) +
                                                       ue_wrap::profile::off::USceneComponent_RelativeRotation);
}

// The engine refuses to move a component that is not Movable once construction is over, so the ball's ends and
// turn keep what its construction script rolled (its begin play's own roll is refused the same way). A write here
// holds the component Movable for the one call and puts its mobility back.
class MovableFor {
public:
    explicit MovableFor(void* comp) {
        off_ = R::FindPropertyOffset(R::ClassOf(comp), L"Mobility");
        if (off_ < 0) return;
        at_ = static_cast<uint8_t*>(comp) + off_;
        was_ = *at_;
        *at_ = 2;  // EComponentMobility::Movable
    }
    ~MovableFor() {
        if (at_) *at_ = was_;
    }
    MovableFor(const MovableFor&) = delete;
    MovableFor& operator=(const MovableFor&) = delete;

private:
    int32_t  off_ = -1;
    uint8_t* at_ = nullptr;
    uint8_t  was_ = 0;
};

// The engine classes' own functions: FindFunction reads one class's list, so each is found on the class declaring it.
void* SceneFn(const wchar_t* name) {
    void* sc = R::FindClass(L"SceneComponent");
    return sc ? R::FindFunction(sc, name) : nullptr;
}

void* SplineFn(const wchar_t* name) {
    void* sc = R::FindClass(L"SplineComponent");
    return sc ? R::FindFunction(sc, name) : nullptr;
}

void SetRelLoc(void* comp, const ue_wrap::FVector& v) {
    ue_wrap::ParamFrame f(SceneFn(L"K2_SetRelativeLocation"));
    if (!f.valid()) return;
    f.Set<ue_wrap::FVector>(L"NewLocation", v);
    f.Set<bool>(L"bTeleport", true);
    MovableFor movable(comp);
    ue_wrap::Call(comp, f);
}
void SetRelRot(void* comp, const ue_wrap::FRotator& r) {
    ue_wrap::ParamFrame f(SceneFn(L"K2_SetRelativeRotation"));
    if (!f.valid()) return;
    f.Set<ue_wrap::FRotator>(L"NewRotation", r);
    f.Set<bool>(L"bTeleport", true);
    MovableFor movable(comp);
    ue_wrap::Call(comp, f);
}

// ---- the host's paths ------------------------------------------------------------------------------------------
// What the host's copy rolled: a ball's turn and ends, or a boo's flight spline in its own (local) space.
bool ReadPath(void* ufo, Kind k, std::vector<ue_wrap::FVector>& out) {
    out.clear();
    if (k == Kind::Ball) {
        void* center = Component(ufo, L"center");
        void* a = Component(ufo, L"a");
        void* b = Component(ufo, L"b");
        if (!center || !a || !b) return false;
        const ue_wrap::FRotator r = RelRot(center);
        out.push_back({r.Pitch, r.Yaw, r.Roll});
        out.push_back(RelLoc(a));
        out.push_back(RelLoc(b));
        return true;
    }
    if (k != Kind::Boo) return false;
    void* spline = Component(ufo, L"flySpline");
    if (!spline) return false;
    void* const numFn = SplineFn(L"GetNumberOfSplinePoints");
    void* const atFn = SplineFn(L"GetLocationAtSplinePoint");
    ue_wrap::ParamFrame num(numFn);
    if (!num.valid() || !ue_wrap::Call(spline, num)) return false;
    const int32_t n = std::min<int32_t>(num.Get<int32_t>(L"ReturnValue"), 255);
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::ParamFrame at(atFn);
        if (!at.valid()) return false;
        at.Set<int32_t>(L"PointIndex", i);
        at.Set<uint8_t>(L"CoordinateSpace", 0);  // local, as gen() adds them
        if (!ue_wrap::Call(spline, at)) return false;
        out.push_back(at.Get<ue_wrap::FVector>(L"ReturnValue"));
    }
    return n > 0;
}

void SendPath(uint32_t eid, Kind k, const std::vector<ue_wrap::FVector>& pts, int slot) {
    if (!g_session) return;
    const uint8_t total = static_cast<uint8_t>(pts.size());
    bool ok = true;
    for (size_t first = 0; first < pts.size(); first += N::kUfoEventMaxPoints) {
        N::UfoEventPayload p{};
        p.eid = eid;
        p.op = k == Kind::Ball ? N::kUfoEventBallPath : N::kUfoEventBooPath;
        p.first = static_cast<uint8_t>(first);
        p.count = static_cast<uint8_t>(std::min<size_t>(N::kUfoEventMaxPoints, pts.size() - first));
        p.total = total;
        for (uint8_t i = 0; i < p.count; ++i) {
            p.v[i][0] = pts[first + i].X;
            p.v[i][1] = pts[first + i].Y;
            p.v[i][2] = pts[first + i].Z;
        }
        ok = (slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::UfoEvent, &p,
                                                        sizeof(p))
                        : g_session->SendReliable(N::ReliableKind::UfoEvent, &p, sizeof(p))) && ok;
    }
    UE_LOGI("ufo_sync: HOST sent %hs eid %u's path, %u point(s), to %hs (ok=%d)", k == Kind::Ball ? "ball" : "boo", eid,
            static_cast<unsigned>(total), slot >= 0 ? "a joiner" : "every client", ok ? 1 : 0);
}

std::vector<ue_wrap::CachedObjRef> g_sent;  // HOST: the UFOs whose path went out
uint64_t g_nextScanAt = 0;

struct ScanCtx {
    int slot;          // -1: every client, new UFOs only; else the joiner, every UFO
    bool snapshot;
};

void VisitUfo(void* ctx, void* obj, int32_t) {
    const ScanCtx& sc = *static_cast<const ScanCtx*>(ctx);
    if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    const Kind k = KindOf(obj);
    if (k != Kind::Ball && k != Kind::Boo) return;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(obj);
    if (eid == EL::kInvalidId) return;  // not enrolled (yet): the next scan
    if (!sc.snapshot) {
        for (const auto& s : g_sent)
            if (s.Is(obj)) return;
    }
    std::vector<ue_wrap::FVector> pts;
    if (!ReadPath(obj, k, pts)) {
        UE_LOGW("ufo_sync: HOST could not read eid %u's path -- its copies fly their own", static_cast<unsigned>(eid));
    } else {
        SendPath(static_cast<uint32_t>(eid), k, pts, sc.slot);
    }
    if (!sc.snapshot) {
        g_sent.emplace_back();
        g_sent.back().Set(obj);
    }
}

void ScanUfos(int slot, bool snapshot) {
    ScanCtx sc{slot, snapshot};
    for (const wchar_t* cls : {L"ufo_ballfo_C", L"ufo_boofo_C", L"ufo_boofo_spawn1_C"})
        if (void* c = OI::ClassByName(cls)) OI::ForEachInstance(c, &VisitUfo, &sc);
}

void TickHost() {
    const uint64_t now = NowMs();
    if (now < g_nextScanAt) return;
    g_nextScanAt = now + 100;
    g_sent.erase(std::remove_if(g_sent.begin(), g_sent.end(), [](const ue_wrap::CachedObjRef& r) { return !r.Alive(); }),
                 g_sent.end());
    ScanUfos(-1, false);
}

// ---- Midas -----------------------------------------------------------------------------------------------------
bool MidasActive(void* midas) {
    int32_t off = -1;
    uint8_t mask = 0;
    return midas && R::FindBoolProperty(R::ClassOf(midas), L"active", off, mask) && off >= 0 &&
           (*(static_cast<uint8_t*>(midas) + off) & mask) != 0;
}

void SendMidas(int slot) {
    if (!g_session) return;
    N::UfoEventPayload p{};
    p.op = N::kUfoEventMidasRun;
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::UfoEvent, &p,
                                                              sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::UfoEvent, &p, sizeof(p));
    UE_LOGI("ufo_sync: HOST sent the Midas run to %hs (ok=%d)", slot >= 0 ? "a joiner" : "every client", ok ? 1 : 0);
}

// runTrigger's watch. HOST: a take-off (the UFO not already flying) goes to every client.
sg::Verdict OnMidasPre(const sg::Call& c) {
    if (IsHost() && c.object && !MidasActive(c.object)) SendMidas(-1);
    return sg::Verdict::Run;
}

// ---- the client's copies ---------------------------------------------------------------------------------------
struct Pending {
    uint32_t eid = 0;
    uint8_t  op = 0;
    uint8_t  total = 0;
    std::vector<ue_wrap::FVector> pts;
    std::vector<bool> got;
    uint64_t since = 0;
};
std::vector<Pending> g_pending;

bool Complete(const Pending& p) {
    return p.total > 0 && std::all_of(p.got.begin(), p.got.end(), [](bool b) { return b; });
}

void ApplyBall(void* ufo, const Pending& p) {
    void* center = Component(ufo, L"center");
    void* a = Component(ufo, L"a");
    void* b = Component(ufo, L"b");
    if (center) SetRelRot(center, {p.pts[0].X, p.pts[0].Y, p.pts[0].Z});
    if (a) SetRelLoc(a, p.pts[1]);
    if (b) SetRelLoc(b, p.pts[2]);
    E::SetActorTickEnabled(ufo, true);  // parked by the lane until this path; now it flies
}

void ApplyBoo(void* ufo, const Pending& p) {
    void* spline = Component(ufo, L"flySpline");
    if (!spline) return;
    {
        ue_wrap::ParamFrame clr(SplineFn(L"ClearSplinePoints"));
        if (!clr.valid()) return;
        clr.Set<bool>(L"bUpdateSpline", false);
        ue_wrap::Call(spline, clr);
    }
    void* const addFn = SplineFn(L"AddSplinePoint");
    for (const ue_wrap::FVector& v : p.pts) {
        ue_wrap::ParamFrame add(addFn);
        if (!add.valid()) return;
        add.Set<ue_wrap::FVector>(L"Position", v);
        add.Set<uint8_t>(L"CoordinateSpace", 0);
        add.Set<bool>(L"bUpdateSpline", false);
        ue_wrap::Call(spline, add);
    }
    if (void* upd = SplineFn(L"UpdateSpline")) R::CallFunction(spline, upd, nullptr);
    ue_wrap::ParamFrame len(SplineFn(L"GetSplineLength"));
    const int32_t lenOff = R::FindPropertyOffset(R::ClassOf(ufo), L"length");
    if (len.valid() && lenOff >= 0 && ue_wrap::Call(spline, len))
        *reinterpret_cast<float*>(static_cast<char*>(ufo) + lenOff) = len.Get<float>(L"ReturnValue");
}

bool TryApply(const Pending& p) {
    void* ufo = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::WorldActor);
    if (!ufo) return false;
    const Kind k = KindOf(ufo);
    if ((p.op == N::kUfoEventBallPath && k != Kind::Ball) || (p.op == N::kUfoEventBooPath && k != Kind::Boo) ||
        (p.op == N::kUfoEventBallPath && p.total != 3)) {
        UE_LOGW("ufo_sync: CLIENT eid %u's path does not fit its copy (%ls) -- dropped", p.eid,
                R::ClassNameOf(ufo).c_str());
        return true;
    }
    if (k == Kind::Ball) ApplyBall(ufo, p);
    else ApplyBoo(ufo, p);
    UE_LOGI("ufo_sync: CLIENT eid %u flies the host's path (%hs, %u point(s))", p.eid, k == Kind::Ball ? "ball" : "boo",
            static_cast<unsigned>(p.total));
    return true;
}

void TickClient() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_pending.size();) {
        const Pending& p = g_pending[i];
        const bool done = Complete(p) && TryApply(p);
        if (done || now - p.since > 10000) {
            if (!done) UE_LOGW("ufo_sync: CLIENT eid %u's path found no copy in 10 s -- dropped", p.eid);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void RunMidas() {
    void* midas = ue_wrap::keyed_objects::Resolve(L"midasufo");
    if (!midas) {
        UE_LOGW("ufo_sync: CLIENT the Midas run found no UFO keyed midasufo");
        return;
    }
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(midas), L"runTrigger"));
    bool ran = false;
    if (f.valid()) {
        f.Set<int32_t>(L"index", 0);
        ran = ue_wrap::Call(midas, f);
    }
    UE_LOGI("ufo_sync: CLIENT the host's Midas UFO took off -- this one's runTrigger ran=%d", ran ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    if (!sg::WatchName(kRowMidasRunTrigger, kTagMidas, &OnMidasPre, nullptr))
        UE_LOGE("ufo_sync: the watch on midasufotest_C::runTrigger was refused -- the Midas run stays the host's alone");
}

void Tick() {
    if (IsHost()) TickHost();
    else if (IsClient()) TickClient();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowMidasRunTrigger, kTagMidas)) return;
    g_settled = true;
    if (sg::NameWatchLive(kRowMidasRunTrigger, kTagMidas)) UE_LOGI("ufo_sync: the watch on midasufotest_C::runTrigger is live");
    else UE_LOGE("ufo_sync: the watch on midasufotest_C::runTrigger is dead -- the Midas run stays the host's alone");
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot < 0) return;
    ScanUfos(slot, true);
    if (MidasActive(ue_wrap::keyed_objects::Resolve(L"midasufo"))) SendMidas(slot);
}

void OnEvent(const coop::net::UfoEventPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kUfoEventMidasRun) {
        RunMidas();
        return;
    }
    if ((p.op != N::kUfoEventBallPath && p.op != N::kUfoEventBooPath) || p.total == 0 || p.count == 0 ||
        p.count > N::kUfoEventMaxPoints || p.first + p.count > p.total) {
        UE_LOGW("ufo_sync: CLIENT a malformed UfoEvent (op %u, %u+%u of %u) -- dropped", p.op, p.first, p.count, p.total);
        return;
    }
    Pending* rec = nullptr;
    for (Pending& q : g_pending)
        if (q.eid == p.eid && q.op == p.op && q.total == p.total) rec = &q;
    if (!rec) {
        g_pending.emplace_back();
        rec = &g_pending.back();
        rec->eid = p.eid;
        rec->op = p.op;
        rec->total = p.total;
        rec->pts.assign(p.total, ue_wrap::FVector{});
        rec->got.assign(p.total, false);
        rec->since = NowMs();
    }
    for (uint8_t i = 0; i < p.count; ++i) {
        rec->pts[p.first + i] = {p.v[i][0], p.v[i][1], p.v[i][2]};
        rec->got[p.first + i] = true;
    }
}

void OnMirrorMaterialized(void* actor) {
    const Kind k = KindOf(actor);
    // The pill and Joel fly timelines with nothing rolled; the boo's own begin play switches its tick on after 5 s.
    if (k == Kind::Pill || k == Kind::Joel) E::SetActorTickEnabled(actor, true);
}

void OnMirrorEnding(void* actor) {
    const Kind k = KindOf(actor);
    // A ball ends its event in ReceiveDestroyed; the others only on their own way out, which the host's destroy skips.
    if (k == Kind::Boo || k == Kind::Pill || k == Kind::Joel) {
        if (coop::creature_event_relay::RetractEntryOf(actor))
            UE_LOGI("ufo_sync: CLIENT the host ended a %ls copy -- its event is ended with it", R::ClassNameOf(actor).c_str());
    }
}

void OnDisconnect() {
    g_sent.clear();
    g_pending.clear();
}

}  // namespace coop::ufo_sync
