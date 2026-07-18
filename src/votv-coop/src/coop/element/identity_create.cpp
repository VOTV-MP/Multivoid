// coop/element/identity_create.cpp -- CreateOrAdopt implementation (the prop-mirror bind keystone).
//
// Moved 2026-06-28 from remote_prop::RegisterPropMirror (sync-consolidation
// refactor, plan section 1). Behavior IDENTICAL to the prior RegisterPropMirror:
// idempotent-adopt / morph-reskin / Install-new with the HEAD live-conflict
// reject. remote_prop::RegisterPropMirror is now a thin forwarder.

#include "coop/element/identity_create.h"

#include <memory>
#include <string>

#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/prop.h"
#include "coop/element/npc.h"          // CreateOrAdoptNpcMirror
#include "coop/element/world_actor.h"  // CreateOrAdoptWorldActorMirror
#include "coop/element/registry.h"
#include "coop/element/element_deleter.h"          // A' v122: the provisional-dissolve deferred drain
#include "coop/creatures/kerfur_entity.h"          // K-5: NotifyKerfurPropMirrorBound (client held-pose eid map)
#include "coop/player/local_streams.h"             // A' v122: held-eid cache rebind fanout (measured held-EDGE-cached)
#include "coop/player/players_registry.h"       // coop::players::kMaxPeers (ownerSlot bound)
#include "coop/element/quiescence_drain.h"      // ArmGhostSweep (v106b: displaced live native -> wholesale adjudication)
#include "coop/props/prop_element_tracker.h"   // RebindLocalElementActor (local-element morph re-skin)
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"                      // IsChipPile (the displaced-native ghost-arm class gate)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"                    // GetActorLocation (identity logs carry loc -- user rule)

namespace coop::element {
// The friended gateway to the sealed MirrorManager::Install (Inc C, 2026-06-29).
// Defined here in coop::element so a wire-mirror bind can ONLY originate from this
// module; the CreateOrAdopt* funnels are its only users. mirror_manager.h friends
// exactly this struct -- Install is private to everyone else (the compile wall).
struct MirrorInstallAccess {
    template <typename T>
    static bool Install(coop::element::MirrorManager<T>& m, coop::element::ElementId eid,
                        std::unique_ptr<T> el, int ownerSlot) {
        return m.Install(eid, std::move(el), ownerSlot);
    }
};
}  // namespace coop::element

namespace {
namespace R = ue_wrap::reflection;

// The canonical owner of ALL Prop Elements (wire mirrors AND prop_element_tracker
// locals AllocAndInstall into this same Instance()).
using coop::element::PropMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// Lossy-narrow a wstring to ASCII for Element name/typeName storage. The VOTV Key
// strings + class names are ASCII in practice (BP-minted NewGuid + class ids).
std::string NarrowAscii(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
    return s;
}

// The single-owner bind decision for a "simple" wire mirror (NPC / WorldActor) --
// no save Key, no morph re-skin, no kerfur held-pose notify (those are prop-only).
// Idempotent-adopt (same actor -> true), HEAD live-conflict reject (a different
// live actor at `eid` -> false), else build the Element + Install at `eid`.
template <typename T>
bool CreateOrAdoptSimpleMirror(coop::element::MirrorManager<T>& mgr,
                               coop::element::ElementId eid, void* actor,
                               const std::wstring& cls, int senderSlot) {
    if (!actor) return false;
    if (auto* existing = coop::element::Registry::Get().Get(eid)) {
        return existing->GetActor() == actor;  // ADOPT idempotent (true) / REJECT divergent (false)
    }
    const int ownerSlot =
        (senderSlot >= 0 && senderSlot < static_cast<int>(coop::players::kMaxPeers))
            ? senderSlot : -1;
    auto mirror = std::make_unique<T>();
    if (!cls.empty()) mirror->SetTypeName(NarrowAscii(cls));
    mirror->SetActor(actor, R::InternalIndexOf(actor));
    return coop::element::MirrorInstallAccess::Install(mgr, eid, std::move(mirror), ownerSlot);
}
}  // namespace

namespace coop::element {

void CreateOrAdoptPropMirror(coop::element::ElementId eid, void* actor,
                             const std::wstring& key, const std::wstring& cls,
                             int senderSlot, bool morph) {
    if (!actor) return;
    // Quiet idempotency (Fork B 2b): under the relaxed snapshot gate the OWNER
    // client re-ingests its own entities at every re-bracket, re-resolving its OWN
    // element's actor and re-binding the same (eid, actor). Short-circuit before
    // Install so the manager's duplicate path doesn't warn once per entity.
    if (auto* existing = coop::element::Registry::Get().Get(eid)) {
        if (existing->GetActor() == actor) return;  // ADOPT: idempotent no-op
        // MORPH V2 -- the SINGLE rebind entry point. Re-skin eid onto the new
        // rendering (pile-A -> clump -> pile-B). HEAD keeps the existing live actor
        // (a different live actor for the same eid is a conflict to reject); the
        // morph LEGITIMATELY swaps the actor (the old one is destroyed right after).
        // Route on the Element's AUTHORITATIVE m_mirror flag -- NOT a caller's
        // runtime guess: a MIRROR rebinds via SetActor here; a LOCAL element (a host
        // applying a client's convert against its OWN pile) MUST go through
        // RebindLocalElementActor so the unified actor->eid reverse (Registry::
        // EidForActor, maintained by NoteActorRebind) stays consistent. Only the
        // morph callers pass true.
        if (morph) {
            if (existing->IsMirror()) {
                existing->SetActor(actor, R::InternalIndexOf(actor));
                // Keep the K-5 client kerfur held-pose map consistent if this is a
                // kerfur mirror (self-filters on class -- no-op for chipPile/clump).
                coop::kerfur_entity::NotifyKerfurPropMirrorBound(actor, eid);
                UE_LOGI("sync::CreateOrAdoptPropMirror: eid=%u REBOUND mirror in place -> actor=%p "
                        "cls='%ls' (morph re-skin)", eid, actor, cls.c_str());
            } else {
                coop::prop_element_tracker::RebindLocalElementActor(eid, actor);
            }
            return;
        }
        // HOST RE-ASSERT REBIND (2026-07-03, docs/piles/12 -- the deny-heal receive half). A HOST-
        // authoritative PropSpawn (senderSlot==0) naming an eid whose MIRROR row here resolves a DIFFERENT
        // actor is the host re-asserting the row's truth: either the row's actor is DEAD (GC churn -- the
        // eid=2947 shape: the row kept a freed pointer) or LIVE-but-foreign (address recycle smeared the row
        // onto another entity -- the eid=3129 shape: 8 identical grab denies, pile wedged until host restart).
        // The old flow fell through to Install, whose duplicate-eid reject made the host's re-assert a silent
        // client-side NO-OP (the 8c13858f audit finding). The host is the identity authority (MTA shape:
        // Packet_EntityAdd for an existing id re-links it, server word absolute) -> re-point the row.
        // 1:1 guard: never steal an actor already bound to a DIFFERENT row -- draining that row on the host's
        // word about THIS eid would smear the other identity; the PropDestroy deny lane owns stale-row drains.
        // The displaced actor is NEVER destroyed (it may be another identity's rendering; the re-bind /
        // FLOOR / twin mechanics own whatever it really is).
        if (senderSlot == 0 && existing->IsMirror()) {
            const coop::element::ElementId prior = Registry::Get().EidForActor(actor);
            if (prior != coop::element::kInvalidId && prior != eid) {
                UE_LOGW("sync::CreateOrAdoptPropMirror: eid=%u host re-assert names actor=%p already bound to "
                        "eid=%u -- REFUSING the cross-row steal (PropDestroy lane owns stale-row drains)",
                        eid, actor, static_cast<unsigned>(prior));
                return;
            }
            void* old = existing->GetActor();
            const bool oldLive = old && R::IsLiveByIndex(old, existing->GetInternalIdx());
            existing->SetActor(actor, R::InternalIndexOf(actor));
            coop::kerfur_entity::NotifyKerfurPropMirrorBound(actor, eid);
            UE_LOGW("sync::CreateOrAdoptPropMirror: eid=%u HOST RE-ASSERT rebound row -> actor=%p key='%ls' "
                    "cls='%ls' (prior actor=%p was %s -- churn/recycle smear healed; displaced actor not "
                    "destroyed)", eid, actor, key.c_str(), cls.c_str(), old,
                    oldLive ? "LIVE-but-foreign" : "dead/stale");
            // v106b: a LIVE displaced native chipPile is now identity-less -- the exact ghost the
            // 10:19:27 wedge left for an E-press to find. Arm the wholesale adjudication (the
            // quiescence_drain reconcile's GHOST-RETIRE tail): its OWN identity gets a re-bind
            // chance in that same pass (binds run before the retire), else it is retired at once.
            if (oldLive && ue_wrap::prop::IsChipPile(old))
                coop::element::quiescence_drain::ArmGhostSweep();
            return;
        }
        // else: fall through to Install, which rejects the duplicate eid (HEAD live-conflict guard).
    }
    // (A') v122 ONE-ACTOR-ONE-ROW invariant (stable-ID root fix,
    // votv-stable-id-no-passive-mint-DESIGN-2026-07-18). The Install below would happily
    // stack a SECOND row onto an actor already bound elsewhere and steal the unified
    // reverse (RegisterMirror overwrites m_byActor) -- the measured zombie/reverse-steal
    // class. Adjudicate by AUTHORITY before Install:
    //   HOST + live local row      -> the host's own element is authoritative; REFUSE.
    //                                 (The corrective -- enroll + re-express under the
    //                                 host eid -- lives at the OnSpawn resolution seam,
    //                                 remote_prop_spawn HostAuthorityHandback_; this wall
    //                                 catches every OTHER path.)
    //   CLIENT + host word (slot 0) -> the DESIGNED identity handback receiver: the
    //                                 provisional client-band local dissolves (wire-silent
    //                                 Take -> ElementDeleter; ~Element's reverse clear is
    //                                 ownership-gated so the mirror's reverse survives the
    //                                 deferred dtor) and the host eid becomes the sole
    //                                 identity. Key index intentionally KEPT (the actor
    //                                 remains its key's live occupant).
    //   CLIENT + peer word          -> never let a peer steal a local row; REFUSE.
    //   prior row is a MIRROR       -> 1:1 conflict (unreachable in 2-peer flows); REFUSE.
    // Every exit logs LOUD (a firing is evidence, not noise -- dead-guard-logs rule).
    {
        const coop::element::ElementId prior = Registry::Get().EidForActor(actor);
        if (prior != coop::element::kInvalidId && prior != eid) {
            Element* pe = Registry::Get().Get(prior);
            if (pe && !pe->IsBeingDeleted() && pe->GetActor() == actor &&
                R::IsLiveByIndex(actor, pe->GetInternalIdx())) {
                if (pe->IsMirror()) {
                    UE_LOGW("sync::CreateOrAdoptPropMirror: eid=%u names actor=%p already MIRRORED under "
                            "eid=%u key='%ls' cls='%ls' -- 1:1 invariant REFUSES the second row",
                            eid, actor, static_cast<unsigned>(prior), key.c_str(), cls.c_str());
                    return;
                }
                if (coop::prop_element_tracker::SessionIsHost()) {
                    UE_LOGW("sync::CreateOrAdoptPropMirror: HOST-AUTHORITY WALL -- refusing mirror eid=%u "
                            "(senderSlot=%d) over the host's own live local eid=%u actor=%p key='%ls' "
                            "cls='%ls' (the OnSpawn handback owns the corrective re-express)",
                            eid, senderSlot, static_cast<unsigned>(prior), actor, key.c_str(), cls.c_str());
                    return;
                }
                if (senderSlot == 0 && coop::prop_element_tracker::SessionIsClient()) {
                    const ue_wrap::FVector dl = ue_wrap::engine::GetActorLocation(actor);
                    UE_LOGW("sync::CreateOrAdoptPropMirror: HANDBACK -- dissolving provisional local "
                            "eid=%u -> adopting host eid=%u actor=%p key='%ls' cls='%ls' loc=(%.1f,%.1f,%.1f)",
                            static_cast<unsigned>(prior), eid, actor, key.c_str(), cls.c_str(),
                            dl.X, dl.Y, dl.Z);
                    if (auto taken = PropMirrors().Take(prior))
                        coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
                    // fall through to Install (the reverse flips to `eid` there; the deferred
                    // ~Element of `prior` cannot clobber it -- registry ownership gate).
                } else {
                    UE_LOGW("sync::CreateOrAdoptPropMirror: eid=%u from peer slot=%d names actor=%p bound "
                            "to local eid=%u -- peer word never dissolves a local; REFUSED",
                            eid, senderSlot, actor, static_cast<unsigned>(prior));
                    return;
                }
            }
        }
    }
    // Tag with the originating peer slot for per-slot disconnect eviction (D1-7).
    // Out-of-range/unknown -> -1 (untagged; only drained on full teardown).
    const int ownerSlot =
        (senderSlot >= 0 && senderSlot < static_cast<int>(coop::players::kMaxPeers))
            ? senderSlot : -1;
    // Build the Prop mirror with name/typeName/actor populated, then hand to
    // MirrorManager::Install (the 5-step pattern: alloc-under-lock + RegisterMirror
    // + rollback-on-fail with dtor outside lock).
    auto mirror = std::make_unique<coop::element::Prop>();
    if (!key.empty()) mirror->SetName(NarrowAscii(key));
    if (!cls.empty()) mirror->SetTypeName(NarrowAscii(cls));
    mirror->SetActor(actor, R::InternalIndexOf(actor));
    if (MirrorInstallAccess::Install(PropMirrors(), eid, std::move(mirror), ownerSlot)) {
        UE_LOGI("sync::CreateOrAdoptPropMirror: eid=%u bound to actor=%p "
                "key='%ls' cls='%ls' ownerSlot=%d",
                eid, actor, key.c_str(), cls.c_str(), ownerSlot);
        // K-5: if this is a CLIENT kerfur prop mirror, record actor->host-range-eid
        // so local_streams can stream the eid while carried. Self-filters to
        // client + kerfur class (no-op for ordinary props / on the host). The single
        // choke-point every kerfur prop mirror bind funnels through.
        coop::kerfur_entity::NotifyKerfurPropMirrorBound(actor, eid);
        // (A') v122 rebind fanout: the held-eid cache re-resolves only at the held
        // EDGE, so an adopt landing MID-carry (incl. the handback dissolve above)
        // would stream a stale/invalid eid for the rest of the hold. One pointer
        // compare when not held.
        coop::local_streams::NotifyPropEidRebound(actor);
    }
    // On false: Install already logged the failure (sentinel id, duplicate, or
    // RegisterMirror failure).
}

bool CreateOrAdoptNpcMirror(coop::element::ElementId eid, void* actor,
                            const std::wstring& cls, int senderSlot) {
    const bool ok = CreateOrAdoptSimpleMirror(
        coop::element::MirrorManager<coop::element::Npc>::Instance(), eid, actor, cls, senderSlot);
    if (ok)
        UE_LOGI("sync::CreateOrAdoptNpcMirror: eid=%u bound to actor=%p cls='%ls'", eid, actor, cls.c_str());
    return ok;
}

bool CreateOrAdoptWorldActorMirror(coop::element::ElementId eid, void* actor,
                                   const std::wstring& cls, int senderSlot) {
    const bool ok = CreateOrAdoptSimpleMirror(
        coop::element::MirrorManager<coop::element::WorldActor>::Instance(), eid, actor, cls, senderSlot);
    if (ok)
        UE_LOGI("sync::CreateOrAdoptWorldActorMirror: eid=%u bound to actor=%p cls='%ls'", eid, actor, cls.c_str());
    return ok;
}

}  // namespace coop::element
