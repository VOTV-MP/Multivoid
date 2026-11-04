// coop/props/prop_settle.cpp -- see coop/props/prop_settle.h.

#include "coop/props/prop_settle.h"

#include "coop/element/quiescence_drain.h"  // SupersedePosCorrection: the settle is the newest pose
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"        // NowMs
#include "coop/props/prop_drive_host.h"     // ContextOf, InAHand: a driven or held prop is not ours to settle
#include "coop/props/prop_drive_rules.h"    // the send epsilons and the rest time
#include "coop/props/prop_hold_claim.h"     // IsArbitrated: the props a hold has one holder of
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_wire_parity.h"    // PhysFlagsOf, ConvergeFrozenSleep
#include "coop/props/remote_prop.h"         // IsOwnedHere, IsHoldReleased, KeyToWString

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"

#include <cmath>
#include <string>
#include <vector>

namespace coop::prop_settle {
namespace {

namespace rules = coop::prop_drive_rules;

// A player drops a handful of props at most in ten seconds; a burst past this falls as it always did.
constexpr size_t kMaxWatched = 16;
// A copy still moving this long after its release has no rest to report, the swinger coast's own bound.
constexpr uint64_t kWatchBoundMs = 10000;

struct Watched {
    ue_wrap::CachedObjRef ref;
    coop::net::WireKey    key{};
    uint16_t              holdGen = 0;
    ue_wrap::FVector      stillLoc{};    // where the current still run began
    ue_wrap::FRotator     stillRot{};
    uint64_t              stillSinceMs = 0;
    uint64_t              deadlineMs = 0;
};
std::vector<Watched> g_watched;

uint32_t g_sent = 0, g_taken = 0, g_unsettled = 0, g_full = 0;
uint32_t g_applied = 0, g_alreadyThere = 0, g_ownedHere = 0, g_notReleased = 0, g_unresolved = 0;

// The prop's own drive on the host -- a hook's tie, a coast, a client's syncer row -- owns where it rests.
bool DrivenByHostChannel(void* actor) {
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId || eid == 0u) return false;
    return coop::prop_drive_host::ContextOf(actor, static_cast<uint32_t>(eid)).open;
}

bool ReadPose(void* actor, ue_wrap::FVector& loc, ue_wrap::FRotator& rot) {
    return ue_wrap::engine::TryGetActorLocation(actor, loc) && ue_wrap::engine::TryGetActorRotation(actor, rot);
}

void Send(coop::net::Session& s, const Watched& w, void* actor, const ue_wrap::FVector& loc,
          const ue_wrap::FRotator& rot) {
    coop::net::PropSettlePayload p{};
    p.key = w.key;
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    p.holdGen = w.holdGen;
    p.physFlags = coop::prop_wire_parity::PhysFlagsOf(actor);
    const bool sent = s.SendReliable(coop::net::ReliableKind::PropSettle, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("prop_settle: hold %u rests at (%.1f, %.1f, %.1f) -- PropSettle %s (#%u)",
            static_cast<unsigned>(w.holdGen), loc.X, loc.Y, loc.Z, sent ? "sent" : "had no peer", g_sent);
}

}  // namespace

void Watch(void* actor, uint16_t holdGen, const coop::net::WireKey& key) {
    if (holdGen == 0 || key.len == 0 || !coop::prop_hold_claim::IsArbitrated(actor)) return;
    if (ue_wrap::prop::IsFrozen(actor) || ue_wrap::prop::IsStatic(actor)) return;  // a slot, a mount: it stays
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!ReadPose(actor, loc, rot)) return;
    for (auto it = g_watched.begin(); it != g_watched.end(); ++it) {
        if (it->ref.Raw() == actor) { g_watched.erase(it); break; }   // a re-release replaces the older watch
    }
    if (g_watched.size() >= kMaxWatched) {
        if (++g_full == 1) UE_LOGI("prop_settle: %zu props already watched -- this one rests as it falls",
                                   kMaxWatched);
        return;
    }
    const uint64_t now = coop::active_drive::NowMs();
    Watched w;
    w.ref.Set(actor);
    w.key = key;
    w.holdGen = holdGen;
    w.stillLoc = loc;
    w.stillRot = rot;
    w.stillSinceMs = now;
    w.deadlineMs = now + kWatchBoundMs;
    g_watched.push_back(w);
}

void Tick(coop::net::Session& session) {
    if (g_watched.empty()) return;
    if (!session.connected()) { g_watched.clear(); return; }
    const uint64_t now = coop::active_drive::NowMs();
    for (auto it = g_watched.begin(); it != g_watched.end();) {
        void* const actor = it->ref.Get();
        if (!actor) { it = g_watched.erase(it); continue; }
        // A hand, a drive or a park took the prop after the release: whoever moves it now owns its rest.
        if (coop::remote_prop::IsOwnedHere(actor) || coop::prop_drive_host::InAHand(actor) ||
            DrivenByHostChannel(actor)) {
            ++g_taken;
            it = g_watched.erase(it);
            continue;
        }
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (ReadPose(actor, loc, rot)) {
            if (rules::MovedPastEps(loc, rot, it->stillLoc, it->stillRot)) {
                it->stillLoc = loc;
                it->stillRot = rot;
                it->stillSinceMs = now;
            } else if (now - it->stillSinceMs >= rules::kRestMs) {
                Send(session, *it, actor, loc, rot);
                it = g_watched.erase(it);
                continue;
            }
        }
        if (now >= it->deadlineMs) {
            ++g_unsettled;
            UE_LOGI("prop_settle: hold %u still moving %llu ms after its release -- no rest to send (#%u)",
                    static_cast<unsigned>(it->holdGen), static_cast<unsigned long long>(kWatchBoundMs),
                    g_unsettled);
            it = g_watched.erase(it);
            continue;
        }
        ++it;
    }
}

void OnSettle(int senderSlot, const coop::net::PropSettlePayload& p) {
    // A settle follows its hold's release on the same lane; a hold not over here, or one the host refused,
    // has no rest of its holder's to take.
    if (!coop::remote_prop::IsHoldReleased(senderSlot, p.holdGen)) {
        ++g_notReleased;
        UE_LOGI("prop_settle: slot %d hold %u is not a released hold here -- settle dropped", senderSlot,
                static_cast<unsigned>(p.holdGen));
        return;
    }
    const std::wstring keyW = coop::remote_prop::KeyToWString(p.key);
    void* const actor = coop::prop_key_index::Find(keyW);
    if (!actor || !coop::prop_hold_claim::IsArbitrated(actor)) {
        ++g_unresolved;
        UE_LOGI("prop_settle: slot %d settle of '%ls' names no prop here -- dropped", senderSlot, keyW.c_str());
        return;
    }
    if (coop::remote_prop::IsOwnedHere(actor) || DrivenByHostChannel(actor)) {
        ++g_ownedHere;
        UE_LOGI("prop_settle: slot %d settle of '%ls' -- a hand or a drive moves it here now; left to that",
                senderSlot, keyW.c_str());
        return;
    }
    const ue_wrap::FVector target{p.locX, p.locY, p.locZ};
    const ue_wrap::FRotator targetRot{p.rotPitch, p.rotYaw, p.rotRoll};
    ue_wrap::FVector cur{};
    ue_wrap::FRotator curRot{};
    const bool haveCur = ReadPose(actor, cur, curRot);
    coop::prop_wire_parity::ConvergeFrozenSleep(actor, p.physFlags);
    if (haveCur && !rules::MovedPastEps(cur, curRot, target, targetRot)) {
        ++g_alreadyThere;
        return;
    }
    ue_wrap::engine::SetActorLocation(actor, target);
    ue_wrap::engine::SetActorRotation(actor, targetRot);
    // A copy still moving here would carry on from the releaser's rest; one already at rest is left asleep,
    // since a velocity write wakes a body.
    if (void* mesh = ue_wrap::prop::GetStaticMesh(actor)) {
        const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(actor);
        const float speedSq = v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                              v.linearCmS.Z * v.linearCmS.Z;
        if (v.ok && speedSq > rules::kRestVelCmS * rules::kRestVelCmS) {
            ue_wrap::engine::SetComponentLinearVelocity(mesh, 0.f, 0.f, 0.f);
            ue_wrap::engine::SetComponentAngularVelocity(mesh, 0.f, 0.f, 0.f);
        }
    }
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid != coop::element::kInvalidId && eid != 0u)
        coop::element::quiescence_drain::SupersedePosCorrection(eid, "prop settle");
    ++g_applied;
    const float dx = target.X - cur.X, dy = target.Y - cur.Y, dz = target.Z - cur.Z;
    UE_LOGI("prop_settle: slot %d hold %u -- '%ls' moved %.1f cm to where its releaser's copy rests (#%u)",
            senderSlot, static_cast<unsigned>(p.holdGen), keyW.c_str(),
            haveCur ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.f, g_applied);
}

void OnDisconnect() {
    if (g_sent || g_taken || g_unsettled || g_full || g_applied || g_alreadyThere || g_ownedHere || g_notReleased ||
        g_unresolved)
        UE_LOGI("prop_settle: session end -- sent=%u taken=%u unsettled=%u full=%u applied=%u already=%u "
                "owned=%u not_released=%u unresolved=%u",
                g_sent, g_taken, g_unsettled, g_full, g_applied, g_alreadyThere, g_ownedHere, g_notReleased,
                g_unresolved);
    g_watched.clear();
    g_sent = g_taken = g_unsettled = g_full = 0;
    g_applied = g_alreadyThere = g_ownedHere = g_notReleased = g_unresolved = 0;
}

}  // namespace coop::prop_settle
