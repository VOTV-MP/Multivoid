// coop/world/agrav_sync.cpp -- see coop/world/agrav_sync.h.

#include "coop/world/agrav_sync.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::agrav_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

// A body whose gravity went off is told lifted once it has risen this far on the host, at the speed it rose; one that
// has not within kRiseWaitMs (blocked, or asleep) is not told.
constexpr float kRiseCm = 3.f;
constexpr uint64_t kRiseWaitMs = 10000;
// A lifted copy rising slower than this has stopped, and is set rising again.
constexpr float kStoppedCmPerSec = 2.f;
// How often the host looks while a trigger holds bodies, and how long it keeps looking once nothing it told is off.
constexpr uint64_t kScanMs = 200;
constexpr uint64_t kQuietMs = 10000;

N::Session* g_session = nullptr;
uint64_t g_nextScan = 0;
uint64_t g_quietSince = 0;
// The `buf` a trigger last held (its data pointer and count): a new gather starts the scan again.
const void* g_lastBuf = nullptr;
int32_t g_lastNum = 0;

// HOST: a body whose gravity went off, from where and when, until it is seen rising (then told) or the wait ends.
struct Pending {
    float z0;
    uint64_t t0;
};
std::unordered_map<uint32_t, Pending> g_pending;
// HOST: the bodies told lifted, by element id.
std::unordered_map<uint32_t, bool> g_lifted;
uint64_t g_rowsLifted = 0, g_rowsDropped = 0;

// CLIENT: the copies the host's float holds up, each with the host's rise in cm/s. A drive row's end (a host coast of
// a body the float knocked) leaves its copy asleep where the host's was, so once a second each one that has stopped,
// and no row drives, is set rising again.
std::unordered_map<uint32_t, float> g_clientLifted;
uint64_t g_nextRelift = 0;
uint64_t g_applied = 0, g_missing = 0, g_relifts = 0;

uint64_t Now() { return ::GetTickCount64(); }

// VOTVCOOP_AGRAV_TRACE=1: every lift and drop, by element id, with the body's height then, on either role (`[AGRAV]`).
bool Trace() {
    static const bool t = coop::config::ReadEnv("VOTVCOOP_AGRAV_TRACE") == "1";
    return t;
}

void TraceOne(const char* what, uint32_t eid, void* actor, float vz) {
    if (!Trace() || !actor) return;
    ue_wrap::FVector at{};
    if (E::TryGetActorLocation(actor, at)) UE_LOGI("[AGRAV] %s eid=%u z=%.1f vz=%.1f", what, eid, at.Z, vz);
}

uint32_t EidOf(void* actor) {
    if (!actor) return 0;
    const coop::element::ElementId e = coop::element::Registry::Get().EidForActor(actor);
    return e == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(e);
}

void* ActorOf(uint32_t eid) {
    return eid ? coop::element::LivePropActor(static_cast<coop::element::ElementId>(eid)) : nullptr;
}

float ZOf(void* actor) {
    ue_wrap::FVector at{};
    return actor && E::TryGetActorLocation(actor, at) ? at.Z : 0.f;
}

// A live trigger's `buf`: TArray<UPrimitiveComponent*>, read in place.
struct Buf {
    void* const* data = nullptr;
    int32_t num = 0;
};

Buf BufOf(void* trigger) {
    static int32_t s_off = -2;
    if (s_off == -2) {
        s_off = R::FindPropertyOffset(R::ClassOf(trigger), L"buf");
        if (s_off < 0) UE_LOGW("agrav_sync: trigger_agrav_C has no `buf` -- the float is not followed");
    }
    Buf b;
    if (s_off < 0) return b;
    const auto* arr = reinterpret_cast<const uint8_t*>(trigger) + s_off;
    b.data = *reinterpret_cast<void* const* const*>(arr);
    b.num = *reinterpret_cast<const int32_t*>(arr + sizeof(void*));
    if (!b.data || b.num < 0 || b.num > 4096) b = Buf{};
    return b;
}

std::vector<void*> Triggers() {
    std::vector<void*> out;
    if (void* const cls = OI::ClassByName(L"trigger_agrav_C"))
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

void Send(bool lifted, const std::vector<N::AgravRow>& rows) {
    if (!g_session || rows.empty()) return;
    for (size_t at = 0; at < rows.size(); at += N::kAgravRowsMax) {
        N::AgravStatePayload p{};
        p.lifted = lifted ? 1 : 0;
        const size_t n = (std::min)(rows.size() - at, static_cast<size_t>(N::kAgravRowsMax));
        p.count = static_cast<uint16_t>(n);
        for (size_t i = 0; i < n; ++i) p.row[i] = rows[at + i];
        if (!g_session->SendReliable(N::ReliableKind::AgravState, &p, sizeof(p)))
            UE_LOGW("agrav_sync: an AgravState of %zu prop(s) was not sent", n);
    }
    (lifted ? g_rowsLifted : g_rowsDropped) += rows.size();
    UE_LOGI("agrav_sync: told %zu prop(s) %s (%llu lifted, %llu dropped in all)", rows.size(),
            lifted ? "lifted" : "dropped", static_cast<unsigned long long>(g_rowsLifted),
            static_cast<unsigned long long>(g_rowsDropped));
}

N::AgravRow Row(uint32_t eid, float vz) {
    N::AgravRow r{};
    r.eid = eid;
    r.vz = static_cast<int16_t>(std::clamp(vz * 10.f, -32000.f, 32000.f));
    return r;
}

void HostScan() {
    const uint64_t now = Now();
    bool fresh = false;
    for (void* t : Triggers()) {
        const Buf b = BufOf(t);
        if (b.data != g_lastBuf || b.num != g_lastNum) {
            g_lastBuf = b.data;
            g_lastNum = b.num;
            fresh = true;
        }
        for (int32_t i = 0; i < b.num; ++i) {
            void* const comp = b.data[i];
            if (!comp || !R::IsLive(comp)) continue;
            void* const actor = R::OuterOf(comp);
            const uint32_t eid = EidOf(actor);
            if (!eid || g_lifted.count(eid) || g_pending.count(eid)) continue;
            // A body that does not simulate here (a frozen prop) is not moved by the float.
            if (!E::IsComponentGravityEnabled(comp) && E::IsComponentSimulatingPhysics(comp))
                g_pending[eid] = Pending{ZOf(actor), now};
        }
    }
    std::vector<N::AgravRow> lift, drop;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        void* const actor = ActorOf(it->first);
        void* const root = actor ? E::RootComponentOf(actor) : nullptr;
        const float rose = actor ? ZOf(actor) - it->second.z0 : 0.f;
        const uint64_t waited = now - it->second.t0;
        if (!root || E::IsComponentGravityEnabled(root) || waited > kRiseWaitMs) {
            it = g_pending.erase(it);   // back on, gone, or never rose: nothing to tell
        } else if (rose > kRiseCm && waited >= kScanMs) {
            const float vz = rose * 1000.f / static_cast<float>(waited);
            lift.push_back(Row(it->first, vz));
            TraceOne("lift", it->first, actor, vz);
            g_lifted[it->first] = true;
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_lifted.begin(); it != g_lifted.end();) {
        void* const actor = ActorOf(it->first);
        void* const root = actor ? E::RootComponentOf(actor) : nullptr;
        if (!root || E::IsComponentGravityEnabled(root)) {
            if (root) {
                drop.push_back(Row(it->first, 0.f));
                TraceOne("drop", it->first, actor, 0.f);
            }
            it = g_lifted.erase(it);
        } else {
            ++it;
        }
    }
    Send(true, lift);
    Send(false, drop);
    if (fresh || !lift.empty() || !drop.empty() || !g_lifted.empty() || !g_pending.empty() || !g_quietSince)
        g_quietSince = now;
}

void Relift() {
    const uint64_t now = Now();
    if (g_clientLifted.empty() || now < g_nextRelift) return;
    g_nextRelift = now + 1000;
    for (const auto& [eid, vz] : g_clientLifted) {
        void* const actor = ActorOf(eid);
        void* const root = actor ? E::RootComponentOf(actor) : nullptr;
        if (!root || coop::remote_prop::DrivingSlotOf(actor) >= 0) continue;
        if (E::IsComponentGravityEnabled(root)) E::SetComponentGravity(root, false);
        // Only a copy that has stopped (a coast's end left it asleep): one still rising keeps its own speed.
        ue_wrap::FVector lin{}, ang{};
        if (E::GetActorRootPhysicsVelocity(actor, lin, ang) && lin.Z > kStoppedCmPerSec) continue;
        E::SetComponentLinearVelocity(root, 0.f, 0.f, vz);
        ++g_relifts;
    }
}

}  // namespace

void Install(N::Session* session) { g_session = session; }

void Tick() {
    if (!g_session) return;
    if (g_session->role() == N::Role::Client) {
        Relift();
        return;
    }
    if (g_session->role() != N::Role::Host || !g_session->connected()) return;
    const uint64_t now = Now();
    if (now < g_nextScan) return;
    // Quiet (nothing told or waiting, and the buffers unchanged for a while): look once a second for a new gather.
    const bool quiet = g_lifted.empty() && g_pending.empty() && g_quietSince && now - g_quietSince > kQuietMs;
    g_nextScan = now + (quiet ? 1000 : kScanMs);
    if (quiet) {
        for (void* t : Triggers()) {
            const Buf b = BufOf(t);
            if (b.data != g_lastBuf || b.num != g_lastNum) { HostScan(); return; }
        }
        return;
    }
    HostScan();
}

void OnState(const N::AgravStatePayload& p) {
    const bool lifted = p.lifted != 0;
    int done = 0, missing = 0;
    for (int i = 0; i < p.count && i < N::kAgravRowsMax; ++i) {
        const uint32_t eid = p.row[i].eid;
        const float vz = static_cast<float>(p.row[i].vz) / 10.f;
        void* const actor = ActorOf(eid);
        void* const root = actor ? E::RootComponentOf(actor) : nullptr;
        if (!root || !E::IsPrimitiveComponent(root)) { ++missing; continue; }
        TraceOne(lifted ? "lift" : "drop", eid, actor, vz);
        E::SetComponentGravity(root, !lifted);
        if (lifted) {
            g_clientLifted[eid] = vz;
            E::SetComponentLinearVelocity(root, 0.f, 0.f, vz);
        } else {
            g_clientLifted.erase(eid);
        }
        ++done;
    }
    g_applied += done;
    g_missing += missing;
    UE_LOGI("agrav_sync: the host %s %d prop(s) here (%d with no copy here)", lifted ? "lifted" : "dropped", done,
            missing);
}

void OnDisconnect() {
    g_pending.clear();
    g_lifted.clear();
    g_clientLifted.clear();
    g_lastBuf = nullptr;
    g_lastNum = 0;
    g_quietSince = 0;
    g_nextScan = 0;
    if (g_rowsLifted || g_applied)
        UE_LOGI("agrav_sync: session over -- told %llu lifted and %llu dropped; applied %llu, %llu with no copy, %llu "
                "lifts set again", static_cast<unsigned long long>(g_rowsLifted),
                static_cast<unsigned long long>(g_rowsDropped), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_missing), static_cast<unsigned long long>(g_relifts));
    g_rowsLifted = g_rowsDropped = g_applied = g_missing = g_relifts = 0;
}

}  // namespace coop::agrav_sync
