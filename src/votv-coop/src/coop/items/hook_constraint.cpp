// coop/items/hook_constraint.cpp -- see coop/items/hook_constraint.h.

#include "coop/items/hook_constraint.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"           // StringFromWireKey
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_element_tracker.h"  // FindLiveActorByKey: the tracked-prop index
#include "coop/props/remote_prop.h"           // ResolveLiveActorByEid: the registry's O(1) row

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/keyed_objects.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::hook_constraint {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};
// Read in the seam callback, written on the game thread at Install and OnDisconnect: true while
// this peer is the CLIENT of a session. The callback's whole gate, so the host and single player
// pay one relaxed load per constraint build.
std::atomic<bool> g_breakHere{false};
bool g_seamInstalled = false;
bool g_seamRefused   = false;

// A tie the seam saw built, waiting for the game thread to judge it. The index is captured at the
// seam, while the actor is known live, so the judgement never probes a bare pointer.
struct PendingBreak {
    void*    actor;
    int32_t  idx;
    void*    comp;
    uint64_t expiresMs;
};
std::vector<PendingBreak> g_pending;   // game thread only
// The class table resolves on the lane's first tick after hook_C loads; a tie built before that
// waits here for it. Five seconds is many times that.
constexpr uint64_t kPendingMs  = 5000;
constexpr size_t   kMaxPending = 64;

// The reach a bite is allowed from the sender's body: the anchor arbiter's number, generous on
// purpose -- a thrown hook is a ballistic arc and the flesh variant's cable is a hundred metres
// -- and it exists to catch a bite across the map, not to police a long shot. Logged, never
// refused.
constexpr float kBiteReachUU = 6000.f;
// A head farther than this from the bitten component's origin is not a bite on that component:
// no prop in the game is that large, and the number is otherwise a constraint offset the sender
// chose. Refused whole, the format rule.
constexpr float kMaxBiteLocalUU = 2000.f;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Finite3(float a, float b, float c) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
}

// v rotated by r: q = r as a quaternion, v' = v + w*t + q x t with t = 2 q x v. The game's own
// setLocs stores the head scale-free (a unit-scale transform of the component's location and
// rotation), so the way back is the same rotation and translation with no scale.
ue_wrap::FVector RotateByRotator(const ue_wrap::FRotator& r, const ue_wrap::FVector& v) {
    float qx = 0.f, qy = 0.f, qz = 0.f, qw = 1.f;
    E::RotatorToQuat(r.Pitch, r.Yaw, r.Roll, qx, qy, qz, qw);
    const float tx = 2.f * (qy * v.Z - qz * v.Y);
    const float ty = 2.f * (qz * v.X - qx * v.Z);
    const float tz = 2.f * (qx * v.Y - qy * v.X);
    return ue_wrap::FVector{v.X + qw * tx + (qy * tz - qz * ty),
                            v.Y + qw * ty + (qz * tx - qx * tz),
                            v.Z + qw * tz + (qx * ty - qy * tx)};
}

void Queue(void* actor, int32_t idx, void* comp) {
    if (g_pending.size() >= kMaxPending) {
        // A client cannot build ties faster than the game lets it; this is a bound on a burst at
        // world load, where every save-loaded hook re-ties inside one frame.
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("hook_constraint: %zu ties queued and unjudged -- dropping the oldest (said once)",
                    g_pending.size());
        }
        g_pending.erase(g_pending.begin());
    }
    g_pending.push_back(PendingBreak{actor, idx, comp, NowMs() + kPendingMs});
}

// The native seam on UPhysicsConstraintComponent::SetConstrainedComponents. `context` is the
// constraint component the call ran on; `sourceObject` is the actor whose bytecode issued it --
// the hook, for a tie its own graph builds. Deep inside the engine's dispatch: no engine call, no
// allocation past the post, and the one field read is of an object the engine holds live for the
// duration of the call.
void OnSetConstrainedPost(void* context, void* sourceObject, void* /*result*/) {
    if (!g_breakHere.load(std::memory_order_relaxed)) return;
    if (!context || !sourceObject) return;
    const int32_t idx = R::InternalIndexOf(sourceObject);
    GT::Post([sourceObject, idx, context] { Queue(sourceObject, idx, context); });
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!GT::IsGameThread()) return;
    g_session.store(session, std::memory_order_release);
    g_breakHere.store(session && session->role() == coop::net::Role::Client,
                      std::memory_order_release);
    if (g_seamInstalled || g_seamRefused) return;
    void* fn = H::SetConstrainedComponentsFunction();
    if (!fn) return;   // retried from Tick
    if (!ue_wrap::ufunction_hook::InstallPostHook(fn, &OnSetConstrainedPost)) {
        g_seamRefused = true;
        UE_LOGE("hook_constraint: the SetConstrainedComponents seam did NOT install (native hook "
                "table full) -- a client's hook keeps its own constraint this session, so a prop it "
                "hooks is pulled on that client alone and diverges");
        return;
    }
    g_seamInstalled = true;
    UE_LOGI("hook_constraint: installed -- UPhysicsConstraintComponent::SetConstrainedComponents "
            "post-seam (a client breaks every hook tie it builds; the host builds a client's tie "
            "on its mirror)");
}

void Tick() {
    if (!GT::IsGameThread()) return;
    if (!g_seamInstalled && !g_seamRefused) Install(Session());
    if (g_pending.empty()) return;
    auto* s = Session();
    const bool client = s && s->connected() && s->role() == coop::net::Role::Client;
    const uint64_t now = NowMs();
    const bool resolved = H::EnsureResolved();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        const PendingBreak& pb = *it;
        if (!client || !R::IsLiveByIndex(pb.actor, pb.idx)) { it = g_pending.erase(it); continue; }
        if (!resolved) {
            if (now < pb.expiresMs) { ++it; continue; }
            UE_LOGW("hook_constraint: a tie on %p was built before the hook classes resolved and "
                    "waited %llu ms -- left standing", pb.actor,
                    static_cast<unsigned long long>(kPendingMs));
            it = g_pending.erase(it);
            continue;
        }
        // The lane's classes only. hook_Child_C, the level-placed variant, is left alone on
        // purpose: it is the level's own furniture, identical on every peer, and what it ties
        // may be a body that is not a prop, which no stream would carry if this side let go of
        // it. A prop it ties is claimed on the host from the world set and parked here, where
        // a client's constraint on a parked body pulls nothing.
        const H::Kind kind = H::KindOf(pb.actor);
        if (kind == H::Kind::Count) { it = g_pending.erase(it); continue; }
        // The A-to-B tie only, not the flight tether: throwConstraint holds the thrower's own
        // body to the sphere that flies, which is the thrower's own expression.
        if (H::PhysicsConstraintOf(pb.actor) != pb.comp) { it = g_pending.erase(it); continue; }
        if (H::BreakConstraint(pb.actor)) {
            UE_LOGI("hook_constraint: CLIENT released the tie on hook %p kind=%u -- the constraint "
                    "lives on the host", pb.actor, static_cast<unsigned>(kind));
        } else {
            UE_LOGW("hook_constraint: BreakConstraint did not dispatch on hook %p kind=%u -- its "
                    "tie stands on this client", pb.actor, static_cast<unsigned>(kind));
        }
        it = g_pending.erase(it);
    }
}

BiteResult BiteMirror(void* mirror, uint8_t ownerSlot, const coop::net::HookStatePayload& p) {
    UE_ASSERT_GAME_THREAD("hook_constraint::BiteMirror");
    auto* s = Session();
    if (!s || !mirror) return BiteResult::Refused;
    if (!(p.flags & coop::net::kHookStateBitten)) return BiteResult::Refused;
    // The host has no mirror of its own hooks, and the reach token is minted for clients only.
    if (ownerSlot == 0 || ownerSlot >= coop::players::kMaxPeers) return BiteResult::Refused;

    const ue_wrap::FVector local{p.blx, p.bly, p.blz};
    if (!Finite3(local.X, local.Y, local.Z) ||
        local.X * local.X + local.Y * local.Y + local.Z * local.Z > kMaxBiteLocalUU * kMaxBiteLocalUU) {
        UE_LOGW("hook_constraint: slot %u seq %u bite refused -- the head offset (%.0f,%.0f,%.0f) "
                "is not on any component", (unsigned)ownerSlot, (unsigned)p.seq, local.X, local.Y,
                local.Z);
        return BiteResult::Refused;
    }
    ue_wrap::FVector normal{p.bnx, p.bny, p.bnz};
    if (!Finite3(normal.X, normal.Y, normal.Z)) return BiteResult::Refused;
    const float n2 = normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z;
    if (n2 < 1e-6f) {
        normal = ue_wrap::FVector{0.f, 0.f, 1.f};
    } else {
        const float inv = 1.f / std::sqrt(n2);
        normal.X *= inv; normal.Y *= inv; normal.Z *= inv;
    }

    // The tail's body: the owner's puppet, which the pose stream drives. Not here yet means the
    // owner's own first pose has not landed either; the next state asks again.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(ownerSlot);
    void* puppet = rp ? rp->GetActor() : nullptr;
    if (!puppet) return BiteResult::Retry;

    // The bitten actor: the element id first (the registry's O(1) row), then the save key through
    // the tracked-prop index, then the game's own key map, which is what makes a keyed thing that
    // is not a prop -- the ATV -- resolve too.
    const std::wstring key = coop::net::StringFromWireKey(p.biteKey);
    void* actor = nullptr;
    const char* how = "";
    if (p.biteEid != 0) {
        actor = coop::remote_prop::ResolveLiveActorByEid(p.biteEid);
        how = "eid";
    }
    if (!actor && !key.empty()) {
        actor = coop::prop_element_tracker::FindLiveActorByKey(key);
        how = "key index";
    }
    if (!actor && !key.empty() && ue_wrap::keyed_objects::Available()) {
        actor = ue_wrap::keyed_objects::Resolve(key.c_str());
        how = "game key map";
    }
    if (!actor || !R::IsLive(actor)) return BiteResult::Retry;

    // The bitten component, by the name the owner's hook holds. A prop's one physics body is its
    // mesh, so a name the host cannot find on a prop ties the mesh and says so: the constraint
    // lands on the body that moves either way.
    const std::wstring compName = coop::net::StringFromWireKey(p.biteComponent);
    void* comp = compName.empty() ? nullptr : H::ComponentByName(actor, compName.c_str());
    if (!comp && PR::IsDescendantOfProp(actor)) {
        comp = PR::GetStaticMesh(actor);
        if (comp)
            UE_LOGW("hook_constraint: slot %u seq %u named component '%ls' of '%ls', which this "
                    "host could not find -- tying the prop's mesh instead",
                    (unsigned)ownerSlot, (unsigned)p.seq, compName.c_str(), key.c_str());
    }
    if (!comp) {
        UE_LOGW("hook_constraint: slot %u seq %u bite refused -- '%ls' has no component '%ls' here",
                (unsigned)ownerSlot, (unsigned)p.seq, key.c_str(), compName.c_str());
        return BiteResult::Refused;
    }

    // Reach, logged and applied: the standing rule is that a discontinuity costs trust, never
    // display, and the anchor arbiter judges its keys the same way.
    {
        const auto tok = coop::element::IntentTarget::ForClientIntent(*s, ownerSlot, kBiteReachUU);
        const coop::element::IntentSubject sub = tok.Authorize(actor);
        if (!sub) {
            UE_LOGW("hook_constraint: slot %u seq %u bit '%ls' %s (reach %.0f, measured %.0f) -- "
                    "APPLIED anyway; bounds are client-scoped and this costs trust, not display",
                    (unsigned)ownerSlot, (unsigned)p.seq, key.c_str(),
                    coop::element::OutcomeName(sub.outcome), sub.reachUU, sub.distUU);
        }
    }

    // The head in this world: the owner's component-frame offset carried back through this
    // host's copy of the component. attach_a places the head at loc + normal, so loc is one
    // normal short of the head.
    const ue_wrap::FVector  cl = E::GetComponentLocation(comp);
    const ue_wrap::FRotator cr = E::GetComponentWorldRotation(comp);
    const ue_wrap::FVector  off = RotateByRotator(cr, local);
    const ue_wrap::FVector  head{cl.X + off.X, cl.Y + off.Y, cl.Z + off.Z};
    const ue_wrap::FVector  loc{head.X - normal.X, head.Y - normal.Y, head.Z - normal.Z};
    const bool thrown = (p.flags & coop::net::kHookStateThrown) != 0;
    if (!H::AttachHead(mirror, actor, comp, loc, normal, puppet, /*checkLen=*/true, thrown)) {
        UE_LOGW("hook_constraint: slot %u seq %u -- attach_a did not dispatch on the mirror",
                (unsigned)ownerSlot, (unsigned)p.seq);
        return BiteResult::Refused;
    }
    UE_LOGI("hook_constraint: HOST tied slot %u seq %u to '%ls' by %s (eid %u, '%ls') head=(%.1f,%.1f,%.1f) "
            "thrown=%d -- the constraint runs here, against the owner's puppet",
            (unsigned)ownerSlot, (unsigned)p.seq, key.c_str(), how, p.biteEid, compName.c_str(),
            head.X, head.Y, head.Z, thrown ? 1 : 0);
    return BiteResult::Tied;
}

void OnDisconnect() {
    g_breakHere.store(false, std::memory_order_release);
    g_pending.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hook_constraint
