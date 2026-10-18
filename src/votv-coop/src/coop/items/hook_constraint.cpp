// coop/items/hook_constraint.cpp -- see coop/items/hook_constraint.h.

#include "coop/items/hook_constraint.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"           // StringFromWireKey
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"      // Find: every live keyed prop by key
#include "coop/props/remote_prop.h"           // ResolveLiveActorByEid: the registry's O(1) row

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::hook_constraint {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// makeAttachments is the one body that builds a hook's A-to-B tie: attach_a calls it after a
// SetConstrainedComponents of its own that the call rebuilds (seven more statements follow it: the
// reference position, the distance and length, the audio, the owner's hooked callback and the
// broadcast; none builds a tie), and the ubergraph calls it for a hook that loads or adopts an anchored tie. All three of PhysicsConstraint's SetConstrainedComponents
// outside attach_a are inside it, and the flight tether, throwConstraint, is built in the throw's
// ubergraph and never here (hook_C's decompiled body).
constexpr int kTagMakeAttachments = 0x484B4D41;  // 'HKMA'
UE_FACT_FN(kRowMakeAttachments, "hook_constraint", L"hook_C", L"makeAttachments", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_watchRegistered = false;
bool     g_watchSettled    = false;
bool     g_saidWatchFail   = false;
uint64_t g_nextWatchMs     = 0;   // the watch is asked about at most once a second

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

// The call has run: the tie stands and no physics step has run with it. A CLIENT breaks it here, in
// the frame that built it, so no client copy of a prop is ever pulled. Broken a frame later, as a
// deferred pass did, the tie ran physics steps first: a client's tie to its own pawn 380 m from the
// prop flung the prop's mirror about 11000 cm per 0.26 s for two seconds, until the host's drive
// parked it back (the hook drill's client-role run). Only the lane's classes: hook_Child_C, the
// level-placed variant that inherits this body, is the level's own furniture, identical on every
// peer, and what it ties may be a body that is not a prop, which no stream would carry if this side
// let go of it. A prop it ties is claimed on the host from the world set and parked here, where a
// client's constraint on a parked body pulls nothing. The body touches PhysicsConstraint, the A-to-B
// tie, and never throwConstraint. Game thread, inside the VM, where engine calls are allowed.
void OnMakeAttachmentsPost(const sg::Call& c) {
    auto* s = Session();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    const H::Kind kind = H::KindOf(c.object);
    if (kind == H::Kind::Count) return;
    const unsigned k = static_cast<unsigned>(kind);
    // Unpaced: the hook body that just ran is the proof the class is loaded, and a tie left standing
    // for want of a resolve is the divergence this watch exists to prevent.
    if (!H::ResolveNow()) {
        UE_LOGW("hook_constraint: a tie on %p was built before the hook members resolved -- left "
                "standing", c.object);
        return;
    }
    if (H::BreakConstraint(c.object)) {
        UE_LOGI("hook_constraint: CLIENT released the tie on hook %p kind=%u -- the constraint "
                "lives on the host", c.object, k);
    } else {
        UE_LOGW("hook_constraint: BreakConstraint did not dispatch on hook %p kind=%u -- its "
                "tie stands on this client", c.object, k);
    }
}

// Register the watch, then ask the gate to resolve its name until it is live or settled dead. At most
// once a second: a registration the gate refuses is said once, and asked again at that pace.
void EnsureWatch() {
    if (g_watchSettled) return;
    // The gate arms at boot or never: asked again, a gate that is not installed refuses the same way
    // for the life of the process.
    if (!sg::IsInstalled()) {
        g_watchSettled = true;
        UE_LOGE("hook_constraint: the script gate is not installed -- no makeAttachments watch this "
                "process, so a client's hook keeps its own constraint and a prop it hooks diverges");
        return;
    }
    const uint64_t now = NowMs();
    if (now < g_nextWatchMs) return;
    g_nextWatchMs = now + 1000;
    // The post judges a hook by its class's name, which is minted before any hook class has loaded.
    if (!H::ResolveNames()) return;
    if (!g_watchRegistered)
        g_watchRegistered = sg::WatchName(kRowMakeAttachments, kTagMakeAttachments, nullptr, &OnMakeAttachmentsPost);
    if (!g_watchRegistered) {
        if (!g_saidWatchFail) {
            g_saidWatchFail = true;
            UE_LOGW("hook_constraint: the gate did not take the makeAttachments watch -- asked again each second");
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowMakeAttachments, kTagMakeAttachments)) {
        g_watchSettled = true;
        UE_LOGI("hook_constraint: installed -- a script-gate watch on hook_C::makeAttachments (a client "
                "breaks every hook tie in the frame that builds it; the host builds a client's tie on "
                "its mirror)");
    } else if (sg::NameWatchSettled(kRowMakeAttachments, kTagMakeAttachments)) {
        g_watchSettled = true;
        UE_LOGE("hook_constraint: the makeAttachments watch is dead -- a client's hook keeps its own "
                "constraint this session, so a prop it hooks is pulled on that client alone and diverges");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!GT::IsGameThread()) return;
    g_session.store(session, std::memory_order_release);
    EnsureWatch();
}

void Tick() {
    if (!GT::IsGameThread()) return;
    EnsureWatch();
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
        actor = coop::prop_key_index::Find(key);
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
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hook_constraint
