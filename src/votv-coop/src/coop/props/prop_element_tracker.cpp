// coop/prop_element_tracker.cpp -- the enrolment owner for keyed props: the processed-Init dedupe
// set, the known-keyed set, MarkPropElement (the one place a local Prop Element is minted, with
// the host's key-uniqueness authority), the actor-to-eid reads, the save-time transform captures,
// the dead-Element reaper and its self-test. The census walk lives in prop_census.cpp and the key
// index in prop_key_index.cpp.

#include "coop/props/prop_element_tracker.h"

#include "prop_element_tracker_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/eid_lifetime_trace.h"  // RecordCaptureEid, the host's capture trace
#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"  // IsKerfurActor, the client-mint gate
#include "coop/net/session.h"
#include "coop/player/hand_item.h"  // hand-axis boundary: CollectHandAxisActors (SeedWalk_ skip; local hand + remote mirrors)
#include "coop/props/prop_synth_key.h"  // MintFreshKeyForDuplicate, the host's re-key of a clone
#include "ue_wrap/engine/engine.h"  // GetActorLocation
#include "ue_wrap/core/game_thread.h"  // IsGameThread; setKey is a dispatch, so the re-key is game-thread gated
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::prop_element_tracker {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// ---- the session pointer ----
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// ---- the processed-Init dedupe set ----
// Steady-state size is the live keyed-interactable count (~2,000 on load); the cap is a backstop,
// and on overflow inserts stop but the set is not cleared (clearing would double-broadcast every
// tracked actor's next super-call). The Init POST and destroy PRE observers run on parallel-anim
// workers, so the set is mutexed; no engine call under the lock.
std::mutex g_processedInitMutex;
std::unordered_set<void*> g_processedInitActors;
constexpr size_t kProcessedInitCap = 16384;
std::atomic<bool> g_processedInitOverflowLogged{false};

// ---- the known-keyed set ----
// The live keyed interactables: seeded once at Install by one GUObjectArray walk, then maintained
// by the Init POST (insert) and the destroy PRE (evict). Engine state, not session state: not
// cleared at disconnect, since actors outlive sessions (MTA's per-type managers keep the same
// live list). The set itself lives at namespace scope (prop_element_tracker_detail.h) so the
// census shares it.
std::atomic<bool> g_knownKeyedPropsOverflowLogged{false};

// ---- the Prop Element ----
// Every Prop Element, this peer's locals and remote_prop's wire mirrors, is owned by the one
// MirrorManager<Prop> (PropMirrors()). Unlike the NPC manager, a Prop manager mixes locals and
// mirrors on one peer; the per-element IsMirror() flag routes each destructor and selects the
// disconnect drain. The actor-to-eid reverse is the unified Registry's (EidForActor), which
// covers locals and mirrors, so GetPropElementIdForActor re-imposes the locals-only contract with
// an IsMirror filter; a two-concurrent-mint race is resolved by MarkPropElement's post-alloc
// double-check. No PRE-to-POST handoff: the Element is created at Init POST, when the actor
// already exists.
using coop::element::PropMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// Whether an actor is a save-loaded native is an Element field (IsSaveNative, set by
// save_identity_bind); an actor-keyed set could read stale relative to the binding.

// The key-to-actor index lives in prop_key_index.cpp; this file reaches it through
// IndexKeyForActor_ and EraseKeyIndexForActor_.

}  // namespace

// The known set, shared with the census walk (prop_element_tracker_detail.h).
std::mutex g_knownKeyedPropsMutex;
std::unordered_set<void*> g_knownKeyedProps;

// ---- setters and role reads ----

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

bool SessionIsHost() {
    auto* s = LoadSession();
    return s != nullptr && s->role() == coop::net::Role::Host;
}

bool SessionIsClient() {
    auto* s = LoadSession();
    return s != nullptr && s->role() == coop::net::Role::Client;
}

// ---- processed-Init accessors ----

void MarkProcessedInit(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    if (g_processedInitActors.size() >= kProcessedInitCap) {
        if (!g_processedInitOverflowLogged.exchange(true)) {
            UE_LOGW("prop_element_tracker: g_processedInitActors hit %zu cap; stopping inserts (Destroy-pruned in steady state -- if this fires we have an Init/Destroy imbalance)",
                    kProcessedInitCap);
        }
        return;
    }
    g_processedInitActors.insert(actor);
}

bool HasProcessedInit(void* actor) {
    if (!actor) return false;
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    return g_processedInitActors.count(actor) > 0;
}

void UnmarkProcessedInit(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    g_processedInitActors.erase(actor);
}

size_t ClearProcessedInit() {
    std::lock_guard<std::mutex> lk(g_processedInitMutex);
    const size_t n = g_processedInitActors.size();
    g_processedInitActors.clear();
    g_processedInitOverflowLogged.store(false);
    return n;
}

// ---- known-keyed maintenance ----

void MarkKnownKeyedProp(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
    if (g_knownKeyedProps.size() >= kKnownKeyedPropsCap) {
        if (!g_knownKeyedPropsOverflowLogged.exchange(true)) {
            UE_LOGW("prop_element_tracker: g_knownKeyedProps hit %zu cap; stopping inserts (snapshot will under-report -- Init/Destroy imbalance bug?)",
                    kKnownKeyedPropsCap);
        }
        return;
    }
    g_knownKeyedProps.insert(actor);
}

void UnmarkKnownKeyedProp(void* actor) {
    if (!actor) return;
    {
        std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
        g_knownKeyedProps.erase(actor);
    }
    // Evict the key index entry first, under its own leaf mutex, only if it still points at this
    // actor (an address recycle that re-indexed a newer prop is left intact).
    EraseKeyIndexForActor_(actor);
    // Resolve the local eid through the Registry reverse (locals only: a mirror actor is not ours
    // to drain), then Take the Element and route it through the ElementDeleter: this observer can
    // fire on a parallel-anim worker, and a worker-instant ~Prop and FreeId would race any
    // in-flight raw pointer. The deferred eid stays allocated until the flush, so a concurrent
    // AllocAndInstall cannot re-pop it. The reverse entry clears in ~Element at the flush, and
    // IsBeingDeleted gates a second Unmark on the same actor.
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || el->IsMirror() || el->IsBeingDeleted()) return;
    if (auto taken = PropMirrors().Take(eid))
        coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
}

// True iff `actor` is a save-loaded native bound as a host-range mirror (the IsSaveNative flag)
// and still the live occupant of its slot; sourced from the Registry reverse and the Element
// field, so it never reads stale relative to the binding, and a recycled address reads false.
bool IsBoundMirrorNative(void* actor) {
    if (!actor) return false;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return false;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || !el->IsSaveNative()) return false;
    return R::IsLiveByIndex(actor, el->GetInternalIdx());  // recycled/dead -> false
}

// ---- MarkPropElement ----
// Role-aware allocation: the host mints in the host range, a client in the peer range; without
// the split both peers popped the host range for unrelated actors, and a client's Registry::Get
// (host eid) resolved to its own local Prop. No session (the boot seed) defaults to the peer
// range, the safe side. Idempotency is a Registry reverse probe, and the commit is the
// AllocAndInstall, which writes the reverse at id assignment; the post-alloc double-check (the
// reverse not equal to our eid) resolves a concurrent mint by dropping ours.
std::wstring MarkPropElement(void* actor, const std::wstring& key, const std::wstring& cls,
                             EnrollSource src) {
    if (!actor) return key;
    // Never mint a local Element on an actor already bound as a save-native mirror: the post-load
    // census walk would otherwise re-localise every bound native. A no-op in normal play.
    if (IsBoundMirrorNative(actor)) return key;
    // A child actor is refused, the game's own rule (Aprop_C::ignoreSave = ignoreSav ||
    // IsChildActor()): its parent's construction script spawns, positions and destroys it on every
    // peer, and its Key is minted per peer, so enrolling one gave it a wire identity (the host
    // broadcast its kerfur eye cameras as world props; the joiner materialised floating cameras and
    // its own were doomed by the sweep). One gate at the one enrolment owner keeps child actors out
    // of the table, the index, the snapshot, the reaper and the sweep on both peers.
    if (ue_wrap::engine::IsChildActor(actor)) {
        UE_LOGI("prop_element_tracker: child-actor enroll REFUSED cls='%ls' key='%ls' actor=%p "
                "(parent-owned sub-actor; never an independent wire identity)",
                cls.c_str(), key.c_str(), actor);
        return key;
    }
    // A client never mints an eid for a kerfur prop: its identity is host-managed (the host owns
    // the KerfurId and the host-range eid; the client adopts the host's mirror through
    // KerfurConvert or the snapshot), and a client mint was the grab-dupe root. The host registers
    // its kerfur props normally. Read outside the lock (reflection only), gated on the same role
    // read the allocation range uses.
    const bool isKerfur = coop::kerfur_entity::IsKerfurActor(actor);
    // Any existing binding for this actor, local or mirror, means it is tracked, so never mint a
    // second local on top; this also blocks a local over a live mirror actor. The early-out makes
    // the key-uniqueness detector below first-enrolment only: a flow that enrols keyed props before
    // the role reads Host must be audited against it (today's only path sets the session before the
    // boot census).
    if (coop::element::Registry::Get().EidForActor(actor) != coop::element::kInvalidId) return key;
    auto* s = LoadSession();
    const bool isHost = (s != nullptr && s->role() == coop::net::Role::Host);
    if (isKerfur && s != nullptr && s->role() == coop::net::Role::Client) return key;  // a client never mints a kerfur
    // No passive mint on a client: a client's census walk must not mint an Element for a keyed
    // prop, since keyed identity is host-authored (a save-loaded prop adopts the host eid by key; a
    // client-born prop mints at its express seam, which broadcasts the identity in the same
    // breath). A silent census mint produced ~2,200 zombie double rows per join. The actor is
    // key-indexed instead (the adopt burst resolves through the index; the sweep's universe reads
    // it). Keyless (chipPile) census mints are untouched; no session keeps minting.
    if (src == EnrollSource::kPassiveCensus && s != nullptr &&
        s->role() == coop::net::Role::Client && !key.empty() && key != L"None") {
        IndexKeyForActor_(actor, key, R::InternalIndexOf(actor));
        return key;
    }
    // The host is the key-uniqueness authority (MTA: the server owns element-id uniqueness). VOTV's
    // own saves ship duplicate interactable Keys (one host save carried 85 trashBitsPile_C across
    // four keys), and every identity layer assumes uniqueness: a clone family funnels its wire rows
    // onto one actor and the join reconcile re-expresses the rest into occupied positions. A keyed
    // actor enrolling while a different live actor carries its Key is re-keyed before anything sees
    // it; the game re-saves the live Key, so the fix persists. A dead incumbent is a loadObjects
    // re-create inheriting its identity, spared by the liveness check. A client never re-keys, and
    // setKey is a dispatch, so off the game thread the duplicate enrols loudly instead.
    std::wstring enrollKey = key;
    if (isHost && !enrollKey.empty() && enrollKey != L"None") {
        void* incumbent = FindLiveActorByKey(enrollKey);
        if (incumbent && incumbent != actor) {
            if (!ue_wrap::game_thread::IsGameThread()) {
                UE_LOGW("prop_element_tracker: KEY-UNIQUENESS tripwire -- duplicate Key '%ls' on '%ls' "
                        "enrolling OFF the game thread (actor=%p incumbent=%p); cannot setKey here -- "
                        "enrolling under the duplicate", enrollKey.c_str(), cls.c_str(), actor, incumbent);
            } else {
                const ue_wrap::FVector dupLoc = ue_wrap::engine::GetActorLocation(actor);
                const std::wstring fresh = coop::prop_synth_key::MintFreshKeyForDuplicate(actor);
                if (!fresh.empty() && fresh != enrollKey) {
                    UE_LOGW("prop_element_tracker: KEY-UNIQUENESS -- second live actor carried Key '%ls': "
                            "'%ls' loc=(%.1f,%.1f,%.1f) re-keyed -> '%ls' (save-born clone family; host key "
                            "authority -- the new key persists via the game's own save)",
                            enrollKey.c_str(), cls.c_str(), dupLoc.X, dupLoc.Y, dupLoc.Z, fresh.c_str());
                    enrollKey = fresh;
                } else {
                    UE_LOGW("prop_element_tracker: KEY-UNIQUENESS -- re-key FAILED for duplicate '%ls' "
                            "key='%ls' loc=(%.1f,%.1f,%.1f) -- enrolling under the duplicate (pre-fix behavior)",
                            cls.c_str(), enrollKey.c_str(), dupLoc.X, dupLoc.Y, dupLoc.Z);
                }
            }
        }
    }
    auto el = std::make_unique<coop::element::Prop>();
    auto toStr = [](const std::wstring& w) {
        std::string s; s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
        return s;
    };
    if (!enrollKey.empty()) el->SetName(toStr(enrollKey));
    if (!cls.empty()) el->SetTypeName(toStr(cls));
    // Capture the GUObjectArray index while the actor is live, so the late-joiner snapshot can
    // validate the pointer with IsLiveByIndex without dereferencing it after a purge; reused to
    // seed the key index.
    const int32_t internalIdx = R::InternalIndexOf(actor);
    el->SetActor(actor, internalIdx);
    // AllocAndInstall into the one manager (the host range when host, the peer range when client;
    // not a mirror). On failure the Element is dropped inside.
    const coop::element::ElementId eid =
        PropMirrors().AllocAndInstall(std::move(el), isHost);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("prop_element_tracker: PropMirrors().AllocAndInstall returned kInvalidId "
                "for actor=%p key='%ls' -- Prop Element not registered "
                "(Registry exhausted / lifetime bug?)",
                actor, enrollKey.c_str());
        return enrollKey;
    }
    // Re-check after the alloc window: AllocAndInstall wrote the reverse at id assignment, and if a
    // concurrent mint of this actor won it, ours is taken back out and routed through the
    // ElementDeleter (this can run on a worker, so no inline FreeId). Exactly one of two racers'
    // writes wins, so exactly one survives.
    if (coop::element::Registry::Get().EidForActor(actor) != eid) {
        if (auto taken = PropMirrors().Take(eid))
            coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
        return enrollKey;
    }
    // Index key to actor for the connect re-snapshot's O(1) wire-key de-dupe, after the commit and
    // only on the winning path, so the index and the reverse stay consistent.
    IndexKeyForActor_(actor, enrollKey, internalIdx);
    return enrollKey;
}

coop::element::ElementId GetPropElementIdForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    // The Registry reverse covers locals and mirrors; this accessor's contract is locals only (the
    // destroy PRE gate and the Init POST stamp act on owned locals), so a mirror actor resolves to
    // kInvalidId.
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return coop::element::kInvalidId;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || el->IsMirror()) return coop::element::kInvalidId;
    return eid;
}

void RebindLocalElementActor(coop::element::ElementId eid, void* newActor) {
    // Game thread only (every morph edge runs there).
    if (eid == coop::element::kInvalidId || eid == 0u || !newActor) return;
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el) return;
    if (el->IsMirror()) {
        // A mirror eid is the wrong path: remote_prop::RegisterPropMirror owns mirror rebinds.
        // Defensive; trash_channel only calls this for a local eid.
        UE_LOGW("prop_element_tracker::RebindLocalElementActor: eid=%u is a MIRROR -- ignoring "
                "(use RegisterPropMirror rebindInPlace)", eid);
        return;
    }
    void* oldActor = el->GetActor();
    if (oldActor == newActor) return;  // idempotent
    // Re-point the Element and its cached liveness index so ResolveLiveActorByEid resolves the new
    // rendering; SetActor maintains the Registry reverse.
    el->SetActor(newActor, R::InternalIndexOf(newActor));
    UE_LOGI("prop_element_tracker::RebindLocalElementActor: eid=%u re-pointed %p -> %p (morph re-skin)",
            eid, oldActor, newActor);
}

void CollectTrackedPileTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // The host's save-time pile positions, keyed by host eid: read right after the scratch save was
    // serialised (save_transfer::OnRequest, the same game-thread tick), so the value equals what
    // the joining client loads its native at. Keyless chipPiles only (a keyed Aprop_C is covered by
    // the key diff). A live pile with no eid is not yet re-minted, not absent: the world-change
    // re-seed can still be deferred at the connect instant, and VOTV's one persistent UWorld keeps
    // the world stamp live, so the seeded-for-world test is no signal; the eid is minted here
    // (register only, idempotent), identical to the one the deferred re-seed later broadcasts. One
    // walk, game thread.
    const int32_t n = R::NumObjects();
    int minted = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsChipPile(obj)) continue;                 // lineage test, pure pointer walks
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
        coop::element::ElementId eid = GetPropElementIdForActor(obj);
        if (eid == coop::element::kInvalidId || eid == 0u) {
            MarkPropElement(obj, L"", R::ClassNameOf(obj),
                            EnrollSource::kPassiveCensus);             // mint now (register-only, idempotent; keyless -> census branch inert)
            eid = GetPropElementIdForActor(obj);                      // resolves in-call (minted before return)
            if (eid == coop::element::kInvalidId || eid == 0u)
                continue;  // mint declined (registry full) -> live-pose fallback for this pile
            ++minted;
        }
        out[eid] = ue_wrap::engine::GetActorLocation(obj);
        coop::dev::eid_lifetime_trace::RecordCaptureEid(obj, static_cast<uint32_t>(eid));  // the capture trace
    }
    if (minted > 0)
        UE_LOGI("prop_element_tracker: CollectTrackedPileTransforms self-seeded %d unseeded live "
                "chipPile(s) (world-change re-seed still deferred at capture) -> %zu pile save-time xform(s)",
                minted, out.size());
}

void CollectTrackedKerfurTransforms(
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& out) {
    // The host's save-time off-form kerfur positions, keyed by host eid: the same capture and
    // self-seed as the piles, gated to the kerfur prop lineage. A kerfur off-prop is a keyed Aprop,
    // so the self-seed mints with its real key. The host stamps this position onto the
    // KerfurConvert when it turns the kerfur on in the join window, and the joining client retires
    // its stale local off-prop at that key. One walk, game thread.
    const int32_t n = R::NumObjects();
    int minted = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::kerfur_entity::IsKerfurPropClass(cls)) continue;  // off-form kerfurs only
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;       // CDO
        coop::element::ElementId eid = GetPropElementIdForActor(obj);
        if (eid == coop::element::kInvalidId || eid == 0u) {
            const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);  // kerfur off-prop is keyed
            // Labelled passive: a census-time walk, not the grab-edge self-seed. Host only in
            // practice; on a client the kerfur gate exits before the mint.
            MarkPropElement(obj, (key == L"None") ? std::wstring() : key, R::ClassNameOf(obj),
                            EnrollSource::kPassiveCensus);
            eid = GetPropElementIdForActor(obj);  // resolves in-call (minted before return)
            if (eid == coop::element::kInvalidId || eid == 0u)
                continue;  // mint declined (registry full) -> no save-time key for this kerfur (live-pose fallback)
            ++minted;
        }
        out[eid] = ue_wrap::engine::GetActorLocation(obj);
        coop::dev::eid_lifetime_trace::RecordCaptureEid(obj, static_cast<uint32_t>(eid));  // the capture trace
    }
    if (minted > 0)
        UE_LOGI("prop_element_tracker: CollectTrackedKerfurTransforms self-seeded %d unseeded live "
                "kerfur off-prop(s) -> %zu kerfur save-time xform(s)", minted, out.size());
}

// ---- the dead-Element reaper ----
// Reaps by eid, not by actor pointer, with an IsMirror gate, so a recycled actor address cannot
// fool it.

size_t ReapDeadLocalPropElements(size_t maxEvictions,
                                 std::vector<coop::element::ElementId>* outReapedEids) {
    if (maxEvictions == 0) return 0;
    // One mutex-guarded copy of (actor, eid, internalIdx, mirror) per Prop Element; no Element*
    // deref after the Registry mutex releases, and the cached index validates each actor without
    // dereferencing a possibly purged pointer.
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(
        coop::element::ElementType::Prop, pairs);
    size_t evicted = 0;
    for (const auto& pr : pairs) {
        if (evicted >= maxEvictions) break;
        if (pr.mirror) continue;                                   // wire mirror -> host's PropDestroy owns it
        if (R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;  // actor alive -> keep
        // A dead local Element. The actor-keyed bookkeeping is cleared only where the Registry
        // reverse still maps this actor to this eid: a recycled address now naming a newer eid must
        // not be disturbed. The reverse entry itself clears in ~Element at the flush.
        const bool ownActorEntry =
            (coop::element::Registry::Get().EidForActor(pr.actor) == pr.id);
        if (ownActorEntry) {
            {
                std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
                g_knownKeyedProps.erase(pr.actor);
            }
            UnmarkProcessedInit(pr.actor);
            // Evict the key index entry too, under the same ownership gate; a rare stale entry
            // under an unused key is harmless (IsLiveByIndex rejects it, and the by-key resolve
            // evicts it lazily).
            EraseKeyIndexForActor_(pr.actor);
        }
        // Take the dead Element out of the manager by eid and defer its destruction to the
        // game-thread flush, UnmarkKnownKeyedProp's drain minus the broadcast; Take returns null if
        // a racing destroy PRE already took it.
        auto taken = PropMirrors().Take(pr.id);
        // The host broadcasts an explicit PropDestroy for this steady-state vanish, which the
        // un-hookable BP path did not replicate. Kerfur props are skipped: their lifecycle is
        // KerfurConvert, and a PropDestroy racing the convert poll's ~200 ms drain would make the
        // client drop and re-convert the kerfur, a dupe. The type name is the signal, since the
        // dead actor cannot be read.
        if (outReapedEids && taken &&
            taken->GetTypeName().find("kerfurOmega") == std::string::npos) {
            outReapedEids->push_back(pr.id);
        }
        coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
        ++evicted;
    }
    if (evicted > 0) {
        UE_LOGI("prop_element_tracker: reaped %zu dead local Prop Element(s) "
                "(mass-purge / level-transition cleanup; scanned %zu Prop Element(s), cap %zu/call)",
                evicted, pairs.size(), maxEvictions);
    }
    return evicted;
}

// ---- the reaper self-test ----

bool DebugCheckPropElementReap() {
    // A sentinel actor address never dereferenced: IsLiveByIndex rejects it on the negative-index
    // fast path, the maps key on the pointer value, and ~Prop does not own the actor.
    void* deadActor = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD0001));
    auto* s = LoadSession();
    const bool isHost = (s != nullptr && s->role() == coop::net::Role::Host);

    // A synthetic dead local Element wired as a real one (the manager and the actor-keyed sets),
    // with internalIdx -1 so the reaper classifies it dead: an actor the engine mass-purged without
    // a K2_DestroyActor.
    auto deadEl = std::make_unique<coop::element::Prop>();
    deadEl->SetName("synth-reap-dead");
    deadEl->SetActor(deadActor, -1);
    const coop::element::ElementId deadEid =
        PropMirrors().AllocAndInstall(std::move(deadEl), isHost);
    if (deadEid == coop::element::kInvalidId) {
        UE_LOGW("propreap_test: FAIL -- AllocAndInstall returned kInvalidId (Registry exhausted?)");
        return false;
    }
    // AllocAndInstall wrote the reverse entry at id assignment.
    MarkKnownKeyedProp(deadActor);
    MarkProcessedInit(deadActor);

    // The live control: a second synthetic local Element bound to a real live UObject (the first
    // live object that is not already a tracked prop), which the reaper must preserve; it is only
    // IsLiveByIndex-checked, never dereferenced, and cleaned up below.
    void* liveActor = nullptr;
    int32_t liveIdx = -1;
    {
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (o && R::IsLive(o) &&
                GetPropElementIdForActor(o) == coop::element::kInvalidId) {
                liveActor = o;
                liveIdx = R::InternalIndexOf(o);
                break;
            }
        }
    }
    coop::element::ElementId liveEid = coop::element::kInvalidId;
    if (liveActor) {
        auto liveEl = std::make_unique<coop::element::Prop>();
        liveEl->SetName("synth-reap-live");
        liveEl->SetActor(liveActor, liveIdx);
        liveEid = PropMirrors().AllocAndInstall(std::move(liveEl), isHost);
        // AllocAndInstall wrote the reverse entry.
    }

    const bool preRegistered = (coop::element::Registry::Get().Get(deadEid) != nullptr) &&
                               (GetPropElementIdForActor(deadActor) == deadEid) &&
                               HasProcessedInit(deadActor);
    if (!preRegistered) {
        UE_LOGW("propreap_test: FAIL -- synthetic dead Element not fully registered pre-reap");
        return false;
    }

    const size_t reaped = ReapDeadLocalPropElements(256);

    // The processed-Init entry is cleared synchronously by the reaper; the reverse entry clears at
    // the deferred flush (checked below with eidFreed).
    const bool initCleared = !HasProcessedInit(deadActor);
    // The live control must survive; read before the cleanup below. Vacuously true only if no live
    // object was found (impossible in a live process).
    const bool livePreserved =
        (liveEid == coop::element::kInvalidId) ||
        (GetPropElementIdForActor(liveActor) == liveEid &&
         coop::element::Registry::Get().Get(liveEid) != nullptr);
    // The eid is freed only when the parked Element destructs: force the game-thread flush now (we
    // are on it) and re-check.
    coop::element::ElementDeleter::Get().Flush();
    // After the flush the dead Element's destructor has run: the eid is freed and the reverse entry
    // cleared.
    const bool eidFreed = (coop::element::Registry::Get().Get(deadEid) == nullptr) &&
                          (GetPropElementIdForActor(deadActor) == coop::element::kInvalidId);

    // Clean up the live control so no synthetic Element stays bound to a random live UObject.
    if (liveEid != coop::element::kInvalidId) {
        if (auto taken = PropMirrors().Take(liveEid))
            coop::element::ElementDeleter::Get().Enqueue(std::move(taken));
        coop::element::ElementDeleter::Get().Flush();
    }

    const bool pass = (reaped >= 1) && initCleared && eidFreed && livePreserved;
    UE_LOGI("propreap_test: forced-dead reap check %s -- reaped=%zu initCleared=%d eidFreed=%d livePreserved=%d (deadEid=%u, liveEid=%u, isHost=%d)",
            pass ? "PASS" : "FAIL", reaped, initCleared ? 1 : 0, eidFreed ? 1 : 0,
            livePreserved ? 1 : 0, deadEid, liveEid, isHost ? 1 : 0);
    return pass;
}

}  // namespace coop::prop_element_tracker
