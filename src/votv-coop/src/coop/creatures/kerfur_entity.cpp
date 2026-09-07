// coop/creatures/kerfur_entity.cpp -- see coop/creatures/kerfur_entity.h.

#include "coop/creatures/kerfur_entity.h"

#include "coop/element/element_deleter.h"  // the kerfur element is queued for deletion, not freed at the seam
#include "coop/element/registry.h"
#include "coop/net/protocol.h"   // KerfurConvertBroadcastPayload + ReliableKind (the BindFormActor wire)
#include "coop/net/session.h"
#include "coop/save/save_transfer.h"  // TryGetSaveTimeKerfurXformAnySlot -- the off-prop pose at the blob instant
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

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

// The resolved kerfur base classes, pushed by the conversion module's install as soon as it
// resolves them. Atomic, so the class tests answer from any thread without taking the table
// mutex below.
std::atomic<void*> g_kerfurNpcClass{nullptr};
std::atomic<void*> g_kerfurPropClass{nullptr};

// The host authority table. Each record's KerfurEntity holds the kerfur id reserved from the
// host allocator for that kerfur's life, and the element destructor frees the id when the
// record is erased. The reverse maps are leaf bookkeeping into the same id.
struct KerfurRecord {
    std::unique_ptr<coop::element::KerfurEntity> elem;  // reserves the host-range kerfur id for its life
    Form                     form        = Form::Npc;
    coop::element::ElementId currentEid  = coop::element::kInvalidId;  // the live Npc/Prop mirror eid
    void*                    actor       = nullptr;
    int32_t                  idx         = -1;
    std::string              npcClassName;
    std::string              propClassName;
    // The host eid of the off-prop this kerfur was at when the host turned it ON during a
    // joiner's load window. Captured at the FIRST conversion (the old eid), gated on that eid
    // being a tracked save off-prop in the blob map, then carried unchanged across every later
    // flip. The NPC spawn builders stamp it as retireOffEid, and the joiner retires the off-prop
    // mirror bound at that eid. Invalid for a kerfur that was never turned on in the window --
    // nothing to retire there.
    coop::element::ElementId originOffEid = coop::element::kInvalidId;
    // The host eid this kerfur most recently converted FROM (the form bind's old eid). The
    // mid-session turn-on NPC spawn carries it as convertFromEid, so the initiating client adopts
    // the conversion ghost it parked under that eid by exact eid instead of spawning a second
    // kerfur beside it. Invalid until the first conversion.
    coop::element::ElementId lastConvertFromEid = coop::element::kInvalidId;
};

std::mutex g_mutex;  // guards every table below
// A client holds none of these: it has no kerfur-id maps and works from wire eids alone.
std::unordered_map<coop::element::ElementId, KerfurRecord> g_byKerfurId;
std::unordered_map<void*, coop::element::ElementId>        g_actorToKerfurId;
std::unordered_map<coop::element::ElementId, coop::element::ElementId> g_eidToKerfurId;  // currentEid -> kerfur id

// The client held-pose map: a kerfur prop MIRROR actor to its host-range eid (see the header).
// Distinct from the host tables above -- these are wire eids, not kerfur ids, and only a client
// fills it. Under g_mutex for uniformity; all access is on the game thread, so contention is nil.
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
    // Idempotent per actor: a repeated registration returns the id already reserved.
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

coop::element::ElementId GetKerfurIdForEid(coop::element::ElementId currentEid) {
    if (currentEid == coop::element::kInvalidId) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_eidToKerfurId.find(currentEid);
    return it == g_eidToKerfurId.end() ? coop::element::kInvalidId : it->second;
}

coop::element::ElementId GetOriginOffEidForEid(coop::element::ElementId currentEid) {
    if (currentEid == coop::element::kInvalidId) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto eit = g_eidToKerfurId.find(currentEid);
    if (eit == g_eidToKerfurId.end()) return coop::element::kInvalidId;
    auto rit = g_byKerfurId.find(eit->second);
    if (rit == g_byKerfurId.end()) return coop::element::kInvalidId;
    return rit->second.originOffEid;
}

coop::element::ElementId GetConvertFromEidForEid(coop::element::ElementId currentEid) {
    if (currentEid == coop::element::kInvalidId) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto eit = g_eidToKerfurId.find(currentEid);
    if (eit == g_eidToKerfurId.end()) return coop::element::kInvalidId;
    auto rit = g_byKerfurId.find(eit->second);
    if (rit == g_byKerfurId.end()) return coop::element::kInvalidId;
    return rit->second.lastConvertFromEid;
}

// ---- The client held-pose map ------------------------------------------------------------

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
    // Self-heal: an actor pointer can be recycled after the mirror that named it was torn down, so
    // verify the eid's element still binds THIS actor; a stale entry is evicted and reads as a
    // miss. (g_mutex then the registry mutex is the lock order the alloc and bind paths already
    // nest.)
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

void ReleaseKerfurForEid(coop::element::ElementId currentEid) {
    if (currentEid == coop::element::kInvalidId) return;
    coop::element::ElementId kerfurId = coop::element::kInvalidId;
    Form form = Form::Npc;
    std::unique_ptr<coop::element::KerfurEntity> drained;
    {
        // One critical section for the lookup AND the erase: a form bind landing between them would
        // move the record to a new eid, and this release would then take a kerfur that had just
        // converted. The currentEid re-check below is what makes that impossible.
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_eidToKerfurId.find(currentEid);
        if (it == g_eidToKerfurId.end()) return;  // not a tracked kerfur's live form
        kerfurId = it->second;
        auto rec = g_byKerfurId.find(kerfurId);
        if (rec == g_byKerfurId.end()) { g_eidToKerfurId.erase(it); return; }  // stale reverse row
        if (rec->second.currentEid != currentEid) return;  // the record has already moved on
        form = rec->second.form;
        if (rec->second.actor) g_actorToKerfurId.erase(rec->second.actor);
        g_eidToKerfurId.erase(it);
        drained = std::move(rec->second.elem);
        g_byKerfurId.erase(rec);
    }
    UE_LOGI("kerfur_entity: releasing K=%u -- its %s form eid=%u died for good",
            static_cast<uint32_t>(kerfurId), form == Form::Npc ? "NPC" : "prop",
            static_cast<uint32_t>(currentEid));
    coop::element::ElementDeleter::Get().Enqueue(std::move(drained));
}

void OnDisconnect() {
    std::unordered_map<coop::element::ElementId, KerfurRecord> drained;  // free the kerfur elements outside the lock
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        drained.swap(g_byKerfurId);
        g_actorToKerfurId.clear();
        g_eidToKerfurId.clear();
        g_kerfurMirrorActorToEid.clear();  // the client held-pose map
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
            // The dying form was never tracked -- a save-loaded prop kerfur the host turns on for
            // the first time. Allocate a fresh stable id now; it carries forward across every later
            // flip.
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
        // Rebind the SAME id onto the NEW form in place; the kerfur id survives the conversion.
        rec.form       = newForm;
        rec.actor      = newActor;
        rec.idx        = newIdx;
        rec.currentEid = newEid;
        const std::string cn = NarrowAscii(className);
        if (newForm == Form::Npc) rec.npcClassName = cn; else rec.propClassName = cn;
        g_actorToKerfurId[newActor] = k;
        g_eidToKerfurId[newEid]     = k;
        // Capture the off-prop's host eid at the FIRST conversion. oldEid is that eid, and the
        // blob-map lookup is the gate that this is a genuine join-window turn-ON of a tracked save
        // off-prop: a turn-OFF's oldEid is an NPC eid, absent from the map, so nothing is captured
        // and no spurious retire is sent. The connect-snapshot NPC spawn builder reads it back and
        // carries it to the joiner as retireOffEid. The KerfurConvert below cannot: a
        // world-mutating reliable does not flow to a slot that has not announced world-ready, which
        // a joiner mid save-transfer has not.
        if (rec.originOffEid == coop::element::kInvalidId) {
            ue_wrap::FVector sv;
            if (coop::save_transfer::TryGetSaveTimeKerfurXformAnySlot(oldEid, sv))
                rec.originOffEid = oldEid;
        }
        // Record the form we converted FROM. A mid-session turn-on's NPC spawn carries this as
        // convertFromEid, so the initiating client adopts the ghost it parked under this eid by
        // exact eid instead of spawning a second kerfur beside it.
        rec.lastConvertFromEid = oldEid;
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
