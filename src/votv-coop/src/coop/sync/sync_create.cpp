// coop/sync/sync_create.cpp -- CreateOrAdopt implementation (the prop-mirror bind keystone).
//
// Moved 2026-06-28 from remote_prop::RegisterPropMirror (sync-consolidation
// refactor, plan section 1). Behavior IDENTICAL to the prior RegisterPropMirror:
// idempotent-adopt / morph-reskin / Install-new with the HEAD live-conflict
// reject. remote_prop::RegisterPropMirror is now a thin forwarder.

#include "coop/sync/sync_create.h"

#include <memory>
#include <string>

#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/kerfur_entity.h"          // K-5: NotifyKerfurPropMirrorBound (client held-pose eid map)
#include "coop/players_registry.h"       // coop::players::kMaxPeers (ownerSlot bound)
#include "coop/prop_element_tracker.h"   // RebindLocalElementActor (local-element morph re-skin)
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

namespace {
namespace R = ue_wrap::reflection;

// The canonical owner of ALL Prop Elements (wire mirrors AND prop_element_tracker
// locals AllocAndInstall into this same Instance()).
inline coop::element::MirrorManager<coop::element::Prop>& PropMirrors() {
    return coop::element::MirrorManager<coop::element::Prop>::Instance();
}

// Lossy-narrow a wstring to ASCII for Element name/typeName storage. The VOTV Key
// strings + class names are ASCII in practice (BP-minted NewGuid + class ids).
std::string NarrowAscii(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
    return s;
}
}  // namespace

namespace coop::sync {

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
        // else: fall through to Install, which rejects the duplicate eid (HEAD live-conflict guard).
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
    if (PropMirrors().Install(eid, std::move(mirror), ownerSlot)) {
        UE_LOGI("sync::CreateOrAdoptPropMirror: eid=%u bound to actor=%p "
                "key='%ls' cls='%ls' ownerSlot=%d",
                eid, actor, key.c_str(), cls.c_str(), ownerSlot);
        // K-5: if this is a CLIENT kerfur prop mirror, record actor->host-range-eid
        // so local_streams can stream the eid while carried. Self-filters to
        // client + kerfur class (no-op for ordinary props / on the host). The single
        // choke-point every kerfur prop mirror bind funnels through.
        coop::kerfur_entity::NotifyKerfurPropMirrorBound(actor, eid);
    }
    // On false: Install already logged the failure (sentinel id, duplicate, or
    // RegisterMirror failure).
}

}  // namespace coop::sync
