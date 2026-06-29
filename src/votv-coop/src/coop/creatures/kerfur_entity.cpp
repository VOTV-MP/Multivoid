// coop/kerfur_entity.cpp -- see coop/kerfur_entity.h.
//
// K-3: the host-side KerfurId authority table + the class/eid predicates. No wire change; the
// conversion still runs the old kerfur_convert path. K-4 wires BindFormActor + the KerfurConvert
// broadcast; K-5 wires the client-mint class-gate + the CLIENT held-pose eid map (no new packet).

#include "coop/creatures/kerfur_entity.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"   // KerfurConvertBroadcastPayload + ReliableKind (the BindFormActor wire)
#include "coop/net/session.h"
#include "coop/session/save_transfer.h"  // scope A: TryGetSaveTimeKerfurXformAnySlot (blob-instant off-prop pos)
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace coop::kerfur_entity {
namespace {

namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// Resolved kerfur base classes (set by kerfur_convert::Install via SetKerfurClasses). Atomic so the
// class-gate predicates can read them from any thread; written once on the game thread at resolve.
std::atomic<void*> g_kerfurNpcClass{nullptr};
std::atomic<void*> g_kerfurPropClass{nullptr};

// HOST authority table. The KerfurEntity unique_ptr reserves K in the Registry (AllocHostId); ~Element
// frees K when the record is erased. The reverse maps are leaf bookkeeping into the same K.
struct KerfurRecord {
    std::unique_ptr<coop::element::KerfurEntity> elem;  // reserves K (host range) for the kerfur's life
    Form                     form        = Form::Npc;
    coop::element::ElementId currentEid  = coop::element::kInvalidId;  // the live Npc/Prop mirror eid
    void*                    actor       = nullptr;
    int32_t                  idx         = -1;
    std::string              npcClassName;
    std::string              propClassName;
    // scope A (kerfur off->active dup retire): the kerfur's blob-instant SAVE-TIME position. Bootstrapped
    // at the FIRST conversion from g_blobKerfurXforms[oldEid] (an off-prop turned ON in the join window),
    // then carried across every subsequent flip on the same K. The KerfurConvert broadcast stamps it as
    // matchX/Y/Z so the joining client retires its stale local off-prop at the exact key.
    float                    saveTimePosX = 0.f, saveTimePosY = 0.f, saveTimePosZ = 0.f;
    bool                     hasSaveTimePos = false;
};

std::mutex g_mutex;  // guards every table below (game thread in practice; locked for the worker-thread predicates)
// HOST authority table (the CLIENT is eid-based -- no KerfurId maps; redesign section 11).
std::unordered_map<coop::element::ElementId, KerfurRecord> g_byKerfurId;
std::unordered_map<void*, coop::element::ElementId>        g_actorToKerfurId;
std::unordered_map<coop::element::ElementId, coop::element::ElementId> g_eidToKerfurId;  // currentEid -> K

// K-5 CLIENT held-pose map: kerfur prop MIRROR actor -> its host-range eid (see the header). Distinct
// from the host KerfurId table above -- this holds wire eids (not K), populated on the client only.
// Guarded by g_mutex for uniformity (all access is game-thread, so contention is nil).
std::unordered_map<void*, coop::element::ElementId> g_kerfurMirrorActorToEid;

std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c));  // class names are ASCII
    return s;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void SetKerfurClasses(void* npcClass, void* propClass) {
    if (npcClass)  g_kerfurNpcClass.store(npcClass, std::memory_order_release);
    if (propClass) g_kerfurPropClass.store(propClass, std::memory_order_release);
}

bool IsKerfurClass(void* cls) {
    if (!cls) return false;
    void* bases[2];
    size_t n = 0;
    if (void* npc  = g_kerfurNpcClass.load(std::memory_order_acquire))  bases[n++] = npc;
    if (void* prop = g_kerfurPropClass.load(std::memory_order_acquire)) bases[n++] = prop;
    if (n == 0) return false;  // classes not resolved yet -> never falsely gate
    return R::IsDescendantOfAny(cls, bases, n);
}

bool IsKerfurActor(void* actor) {
    return actor && IsKerfurClass(R::ClassOf(actor));
}

bool IsKerfurPropClass(void* cls) {
    if (!cls) return false;
    void* prop = g_kerfurPropClass.load(std::memory_order_acquire);
    if (!prop) return false;  // not resolved yet -> never falsely gate
    return R::IsDescendantOfAny(cls, &prop, 1);
}

coop::element::ElementId AllocKerfurId(void* actor, coop::element::ElementId currentEid,
                                       Form form, const std::wstring& className) {
    if (!actor) return coop::element::kInvalidId;
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return coop::element::kInvalidId;  // host = sole authority
    std::lock_guard<std::mutex> lk(g_mutex);
    // Idempotent per actor (a re-scan / repeated registration returns the existing K).
    auto ait = g_actorToKerfurId.find(actor);
    if (ait != g_actorToKerfurId.end()) return ait->second;

    auto ent = std::make_unique<coop::element::KerfurEntity>();
    const coop::element::ElementId k = coop::element::Registry::Get().AllocHostId(ent.get());
    if (k == coop::element::kInvalidId) {
        UE_LOGW("kerfur_entity: AllocHostId exhausted -- cannot reserve KerfurId for actor %p", actor);
        return coop::element::kInvalidId;
    }
    KerfurRecord rec;
    rec.elem       = std::move(ent);
    rec.form       = form;
    rec.currentEid = currentEid;
    rec.actor      = actor;
    rec.idx        = R::InternalIndexOf(actor);
    const std::string cn = NarrowAscii(className);
    if (form == Form::Npc) rec.npcClassName = cn; else rec.propClassName = cn;

    g_actorToKerfurId[actor] = k;
    if (currentEid != coop::element::kInvalidId) g_eidToKerfurId[currentEid] = k;
    g_byKerfurId[k] = std::move(rec);
    UE_LOGI("kerfur_entity: reserved KerfurId=%u for %s kerfur actor=%p currentEid=%u class='%ls'",
            k, form == Form::Npc ? "NPC" : "prop", actor, currentEid, className.c_str());
    return k;
}

coop::element::ElementId GetKerfurIdForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_actorToKerfurId.find(actor);
    return it == g_actorToKerfurId.end() ? coop::element::kInvalidId : it->second;
}

coop::element::ElementId GetKerfurIdForEid(coop::element::ElementId currentEid) {
    if (currentEid == coop::element::kInvalidId) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_eidToKerfurId.find(currentEid);
    return it == g_eidToKerfurId.end() ? coop::element::kInvalidId : it->second;
}

coop::element::ElementId GetCurrentEidForKerfurId(coop::element::ElementId kerfurId) {
    if (kerfurId == coop::element::kInvalidId) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_byKerfurId.find(kerfurId);
    return it == g_byKerfurId.end() ? coop::element::kInvalidId : it->second.currentEid;
}

Form GetFormForKerfurId(coop::element::ElementId kerfurId) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_byKerfurId.find(kerfurId);
    return it == g_byKerfurId.end() ? Form::Npc : it->second.form;
}

bool IsKerfurEid(coop::element::ElementId currentEid) {
    if (currentEid == coop::element::kInvalidId) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_eidToKerfurId.count(currentEid) != 0;
}

bool GetSaveTimePosForEid(coop::element::ElementId currentEid, float& outX, float& outY, float& outZ) {
    // scope A v1: the blob-instant SAVE-TIME position of the kerfur currently at `currentEid`, IFF it was
    // bootstrapped at a join-window turn-on (BindFormActor stamped rec.saveTimePos from g_blobKerfurXforms).
    // A kerfur that was already ACTIVE at blob (never converted) has no anchor -> returns false -> the npc
    // EntitySpawn builder leaves hasMatchPos=0 (nothing to retire). Host-side; HOST only in practice.
    if (currentEid == coop::element::kInvalidId) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto eit = g_eidToKerfurId.find(currentEid);
    if (eit == g_eidToKerfurId.end()) return false;
    auto rit = g_byKerfurId.find(eit->second);
    if (rit == g_byKerfurId.end() || !rit->second.hasSaveTimePos) return false;
    outX = rit->second.saveTimePosX; outY = rit->second.saveTimePosY; outZ = rit->second.saveTimePosZ;
    return true;
}

// ---- K-5 CLIENT held-pose map -----------------------------------------------------------------------

void NotifyKerfurPropMirrorBound(void* actor, coop::element::ElementId eid) {
    if (!actor || eid == coop::element::kInvalidId) return;
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Client) return;  // client-only (host owns the kerfur as a local)
    if (!IsKerfurActor(actor)) return;                        // self-filter: only kerfur prop mirrors
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_kerfurMirrorActorToEid[actor] = eid;
    }
    UE_LOGI("kerfur_entity[client]: kerfur prop mirror actor=%p bound at host-range eid=%u "
            "(held-pose stream can now carry it)", actor, eid);
}

coop::element::ElementId GetKerfurMirrorEidForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_kerfurMirrorActorToEid.find(actor);
    if (it == g_kerfurMirrorActorToEid.end()) return coop::element::kInvalidId;
    const coop::element::ElementId eid = it->second;
    // Self-heal: an actor pointer can be GC-recycled after the mirror it named was torn down. Verify
    // the eid's Element still binds THIS actor; if not, the entry is stale -> evict + miss. (g_mutex ->
    // Registry::m_mutex is the established lock order -- AllocKerfurId/BindFormActor already nest it.)
    auto* el = coop::element::Registry::Get().Get(eid);
    if (el && el->GetActor() == actor) return eid;
    g_kerfurMirrorActorToEid.erase(it);
    return coop::element::kInvalidId;
}

void ForgetKerfurPropMirror(void* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_kerfurMirrorActorToEid.erase(actor);
}

void ReleaseKerfurId(coop::element::ElementId kerfurId) {
    if (kerfurId == coop::element::kInvalidId) return;
    std::unique_ptr<coop::element::KerfurEntity> drained;  // free K's Element OUTSIDE the lock
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_byKerfurId.find(kerfurId);
        if (it != g_byKerfurId.end()) {
            if (it->second.actor) g_actorToKerfurId.erase(it->second.actor);
            if (it->second.currentEid != coop::element::kInvalidId)
                g_eidToKerfurId.erase(it->second.currentEid);
            drained = std::move(it->second.elem);
            g_byKerfurId.erase(it);
        }
    }
    // drained's ~KerfurEntity (-> ~Element -> Registry::FreeId(K)) fires here, outside g_mutex.
}

void OnDisconnect() {
    std::unordered_map<coop::element::ElementId, KerfurRecord> drained;  // free K Elements outside the lock
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        drained.swap(g_byKerfurId);
        g_actorToKerfurId.clear();
        g_eidToKerfurId.clear();
        g_kerfurMirrorActorToEid.clear();  // K-5 client held-pose map
    }
    // drained's KerfurEntity dtors (FreeId) fire here, outside g_mutex.
}

coop::element::ElementId BindFormActor(coop::element::ElementId oldEid, void* newActor,
                                       int32_t newIdx, coop::element::ElementId newEid, Form newForm,
                                       const std::wstring& className,
                                       float locX, float locY, float locZ,
                                       float rotPitch, float rotYaw, float rotRoll) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return coop::element::kInvalidId;
    if (!newActor || newEid == coop::element::kInvalidId) return coop::element::kInvalidId;

    coop::element::ElementId k = coop::element::kInvalidId;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto eit = g_eidToKerfurId.find(oldEid);
        if (eit != g_eidToKerfurId.end()) k = eit->second;
        if (k == coop::element::kInvalidId) {
            // The dying form was never tracked (a save-loaded prop kerfur turned on for the first time
            // -- prop first-sighting lands in K-5). Alloc a fresh stable K now; it carries forward.
            auto ent = std::make_unique<coop::element::KerfurEntity>();
            k = coop::element::Registry::Get().AllocHostId(ent.get());
            if (k == coop::element::kInvalidId) {
                UE_LOGW("kerfur_entity: BindFormActor AllocHostId exhausted -- cannot bind newEid=%u",
                        newEid);
                return coop::element::kInvalidId;
            }
            KerfurRecord fresh;
            fresh.elem = std::move(ent);
            g_byKerfurId[k] = std::move(fresh);
        }
        KerfurRecord& rec = g_byKerfurId[k];
        // Drop the OLD form's reverse-map entries. The old actor died; oldEid's Npc/Prop Element is
        // released SILENTLY by the CALLER (not here -- this module owns only the KerfurId table).
        if (rec.actor) g_actorToKerfurId.erase(rec.actor);
        if (rec.currentEid != coop::element::kInvalidId) g_eidToKerfurId.erase(rec.currentEid);
        g_eidToKerfurId.erase(oldEid);  // defensive: oldEid may differ from rec.currentEid
        // Rebind the SAME K onto the NEW form IN PLACE (K preserved across the conversion).
        rec.form       = newForm;
        rec.actor      = newActor;
        rec.idx        = newIdx;
        rec.currentEid = newEid;
        const std::string cn = NarrowAscii(className);
        if (newForm == Form::Npc) rec.npcClassName = cn; else rec.propClassName = cn;
        g_actorToKerfurId[newActor] = k;
        g_eidToKerfurId[newEid]     = k;
        // scope A v1: bootstrap the save-time anchor from the blob map at the FIRST conversion (oldEid is
        // the off-prop eid captured in g_blobKerfurXforms when a join-window turn-on fires); carry it
        // forward across every subsequent flip on this K. The connect-snapshot npc EntitySpawn builder
        // reads it via GetSaveTimePosForEid and carries it to the joiner (NOT the KerfurConvert, whose
        // SendReliable fails mid-join -- hands-on 16:37).
        if (!rec.hasSaveTimePos) {
            ue_wrap::FVector sv;
            if (coop::save_transfer::TryGetSaveTimeKerfurXformAnySlot(oldEid, sv)) {
                rec.saveTimePosX = sv.X; rec.saveTimePosY = sv.Y; rec.saveTimePosZ = sv.Z;
                rec.hasSaveTimePos = true;
            }
        }
    }

    // Broadcast the SOLE conversion-transition packet (host fan-out to all peers).
    coop::net::KerfurConvertBroadcastPayload p{};
    p.kerfurId = static_cast<uint32_t>(k);
    p.oldEid   = static_cast<uint32_t>(oldEid);
    p.newEid   = static_cast<uint32_t>(newEid);
    p.toForm   = (newForm == Form::Prop) ? 1u : 0u;
    p.rejected = 0;
    p.locX = locX; p.locY = locY; p.locZ = locZ;
    p.rotPitch = rotPitch; p.rotYaw = rotYaw; p.rotRoll = rotRoll;
    p.newClassName.len = 0;
    for (size_t i = 0; i < className.size() && i < 63; ++i)
        p.newClassName.data[p.newClassName.len++] = static_cast<char>(className[i]);
    if (!s->SendReliable(coop::net::ReliableKind::KerfurConvert, &p, sizeof(p))) {
        UE_LOGW("kerfur_entity: SendReliable(KerfurConvert) failed K=%u oldEid=%u newEid=%u",
                k, oldEid, newEid);
    }
    UE_LOGI("kerfur_entity: BindFormActor K=%u %s oldEid=%u -> newEid=%u actor=%p class='%ls' (KerfurConvert broadcast)",
            k, newForm == Form::Npc ? "->NPC(turn-on)" : "->prop(turn_off)",
            oldEid, newEid, newActor, className.c_str());
    return k;
}

void BroadcastConvertRejected(coop::element::ElementId oldEid, Form oldForm,
                              float locX, float locY, float locZ,
                              float rotPitch, float rotYaw, float rotRoll,
                              const std::wstring& className) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    coop::element::ElementId k;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto eit = g_eidToKerfurId.find(oldEid);
        k = (eit == g_eidToKerfurId.end()) ? coop::element::kInvalidId : eit->second;
    }
    coop::net::KerfurConvertBroadcastPayload p{};
    p.kerfurId = static_cast<uint32_t>(k);
    p.oldEid   = static_cast<uint32_t>(oldEid);
    p.newEid   = static_cast<uint32_t>(oldEid);  // unchanged -- the kerfur stays at oldEid in its old form
    p.toForm   = (oldForm == Form::Prop) ? 1u : 0u;  // the form to RESTORE on an optimistic client
    p.rejected = 1;
    p.locX = locX; p.locY = locY; p.locZ = locZ;
    p.rotPitch = rotPitch; p.rotYaw = rotYaw; p.rotRoll = rotRoll;
    p.newClassName.len = 0;
    for (size_t i = 0; i < className.size() && i < 63; ++i)
        p.newClassName.data[p.newClassName.len++] = static_cast<char>(className[i]);
    if (!s->SendReliable(coop::net::ReliableKind::KerfurConvert, &p, sizeof(p))) {
        UE_LOGW("kerfur_entity: SendReliable(KerfurConvert rejected) failed oldEid=%u", oldEid);
    }
    UE_LOGI("kerfur_entity: BroadcastConvertRejected K=%u oldEid=%u form=%s (host refused -- clients restore their mirror)",
            k, oldEid, oldForm == Form::Npc ? "NPC" : "prop");
}

}  // namespace coop::kerfur_entity
