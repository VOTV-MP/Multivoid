// coop/creatures/kerfur_convert_client.cpp -- the client half of the kerfur conversion:
// conversion-ghost custody (claim and park, cleanup reap, take by eid) and the KerfurConvert
// wire apply. Interface: coop/creatures/kerfur_convert_client.h; the feature narrative is in
// kerfur_convert.h. The class pointers and the session arrive from the kerfur_convert install.

#include "coop/creatures/kerfur_convert_client.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::kerfur_convert_client {
namespace {

namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// The resolved kerfur classes, pushed by the install on every attempt; read on the game thread.
void* g_kerfurNpcClass  = nullptr;
void* g_kerfurPropClass = nullptr;
void* g_floppyClass     = nullptr;

// The client-side apply: materialise, or adopt this peer's claimed local ghost into, a kerfur
// mirror of the form at host-range `eid`. `adoptEid` is the converting mirror's eid (the host's
// oldEid): the parked ghost tagged with it is adopted by that eid, deterministically; an invalid
// id means no eid-keyed adopt (the rejected-restore path). NPC: adopt the parked turn-on ghost,
// else fresh-spawn. Prop: adopt the parked turn-off ghost, else drive the PropSpawn receiver.
// The prop mirror carries a synthetic key derived from the eid.
void MaterializeKerfurMirror(bool toNpc, coop::element::ElementId eid, coop::element::ElementId adoptEid,
                             const std::wstring& classW,
                             float lx, float ly, float lz, float rp, float ry, float rr,
                             void* localPlayer) {
    if (toNpc) {
        // Adopt the eid-tagged ghost: the initiator's ghost is always tagged with the converting
        // eid, and a non-initiator has no ghost at it, so it fresh-spawns below, eid-deduped.
        void* ghost = (adoptEid != coop::element::kInvalidId)
                          ? TakeParkedGhostByEid(static_cast<uint32_t>(adoptEid), /*wantNpc=*/true)
                          : nullptr;
        if (ghost) {
            if (coop::npc_mirror::AdoptExistingNpcAsMirror(ghost, static_cast<uint32_t>(eid), classW)) {
                UE_LOGI("kerfur_convert[client]: adopted parked turn-on ghost as NPC mirror eid=%u (by eid)", eid);
                return;
            }
            // The adopt failed on a duplicate eid: fall through to a fresh spawn beside it; the
            // cleanup reaps the ghost.
        }
        void* actorClass = R::FindClass(classW.c_str());
        if (!actorClass) {
            UE_LOGW("kerfur_convert[client]: cannot resolve class '%ls' for NPC materialize eid=%u",
                    classW.c_str(), eid);
            return;
        }
        coop::npc_mirror::SpawnFreshNpcMirror(classW, actorClass, static_cast<uint32_t>(eid),
                                              lx, ly, lz, rp, ry, rr);
    } else {
        const std::string key8 = "coopkerfur#" + std::to_string(static_cast<unsigned>(eid));
        // If our own turn-off parked a prop ghost tagged with the converting eid, adopt that exact
        // actor as the host-eid mirror and snap it to the authoritative pose.
        if (adoptEid != coop::element::kInvalidId) {
            if (void* ghost = TakeParkedGhostByEid(static_cast<uint32_t>(adoptEid), /*wantNpc=*/false)) {
                std::wstring key8w(key8.begin(), key8.end());
                coop::remote_prop::RegisterPropMirror(eid, ghost, key8w, classW, /*senderSlot=*/0,
                                                      /*rebindInPlace=*/false);
                ue_wrap::engine::SetActorLocation(ghost, ue_wrap::FVector{lx, ly, lz});
                ue_wrap::engine::SetActorRotation(ghost, ue_wrap::FRotator{rp, ry, rr});
                // The claim does not freeze a prop ghost, so it has already fallen and settled by
                // local physics during the host round-trip, as in single player; the adopt is the
                // pose snap above, sub-meter on the same floor.
                UE_LOGI("kerfur_convert[client]: adopted parked turn-off ghost as PROP mirror eid=%u (by eid -- "
                        "deterministic, no fuzzy miss; settled by local physics, snapped to host pose)", eid);
                return;
            }
        }
        // No eid-tagged ghost (a non-initiator, or the rejected-restore path): drive the PropSpawn
        // receiver, which adopts by exact key or fuzzy match, else fresh-spawns, and registers the
        // mirror. The key is synthetic, from the eid.
        coop::net::PropSpawnPayload sp{};
        sp.className.len = 0;
        for (size_t i = 0; i < classW.size() && i < 63; ++i)
            sp.className.data[sp.className.len++] = static_cast<char>(classW[i]);
        sp.key.len = 0;
        for (size_t i = 0; i < key8.size() && i < 31; ++i) sp.key.data[sp.key.len++] = key8[i];
        sp.propName.len = 0;
        sp.locX = lx; sp.locY = ly; sp.locZ = lz;
        sp.rotPitch = rp; sp.rotYaw = ry; sp.rotRoll = rr;
        sp.scaleX = sp.scaleY = sp.scaleZ = 1.f;
        sp.physFlags = 0;
        sp.chipType = 0;
        sp.elementId = static_cast<uint32_t>(eid);
        // Both flags are passed explicitly. deferKerfur must be false: the parked ghost is ready
        // now, so OnSpawn must run the inline adopt rather than arm the join-time kerfur prop
        // adopter, which is for un-converted save-loaded twins and cannot match a conversion ghost
        // (its fresh per-load key fails the anti-collision gate), so it fresh-spawned a second prop
        // beside the orphaned ghost, a visible double.
        coop::remote_prop_spawn::OnSpawn(sp, /*senderSlot=*/0, localPlayer,
                                         /*fromConvert=*/false, /*deferKerfur=*/false);
    }
}

// Conversion-ghost custody. The client's own toggle spawns the new-form actor through the
// EX_CallMath path ProcessEvent cannot see, a live untracked ghost we cannot prevent.
// Destroying it traded the dupe for a destroy-respawn pop and still lost the race to a grab: an
// untracked ghost, once grabbed, is broadcast as a new client entity the host mirrors. So no
// untracked ghost is ever left: the instant the poll detects our toggle it claims the ghost
// (the NPC is parked so its AI and physics stop; the prop is left to settle by local physics),
// then adopts it as the host mirror by the exact converting eid when the authoritative entity
// arrives, the NPC in the EntitySpawn receiver and the prop in MaterializeKerfurMirror. A ghost
// the host never confirms is destroyed by CleanupParkedGhosts after a timeout, so it can never
// be grabbed.
struct ParkedGhost {
    void*    actor;
    int32_t  idx;
    uint8_t  toProp;  // 1 = prop ghost (turn-off result), 0 = NPC ghost (turn-on result)
    uint32_t srcEid;  // the eid of the mirror that converted; the host's KerfurConvert carries it as oldEid
    std::chrono::steady_clock::time_point armed;
};
std::vector<ParkedGhost> g_parkedGhosts;  // GT-only (poll + OnEntitySpawn are both game thread)

// Collect the actors bound to wire-mirror Elements of the requested types. This is the
// authoritative host-owns-this-actor signal, not the per-actor reverse maps: a prop adopt rekey
// and an NPC mirror install bind the ghost as a mirror Element without updating those maps, so
// the maps read a just-adopted ghost as untracked, and the cleanup once destroyed a freshly
// adopted prop, which the poll saw as a death and answered with a spurious turn-on. Built once
// per caller under the manager's leaf mutex, then constant-time lookups.
void CollectMirrorActors(bool wantProp, bool wantNpc, std::unordered_set<void*>& out) {
    if (wantProp) {
        std::vector<coop::element::Prop*> v;
        coop::element::MirrorManager<coop::element::Prop>::Instance().Snapshot(v);
        for (auto* el : v)
            if (el && el->IsMirror() && el->GetActor()) out.insert(el->GetActor());
    }
    if (wantNpc) {
        std::vector<coop::element::Npc*> v;
        coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(v);
        for (auto* el : v)
            if (el && el->IsMirror() && el->GetActor()) out.insert(el->GetActor());
    }
}

}  // namespace

// Find and park the client's own just-spawned conversion ghosts: untracked live kerfurs of the
// new form near the site. A turn-on spawns one NPC body; a turn-off can drop the prop and its
// carried floppy. One cold-path object-array walk, only on a detected client toggle.
void ClaimConversionGhosts(uint32_t srcEid, bool wantNpc, float x, float y, float z) {
    void* bases[2];
    size_t nBases = 0;
    if (wantNpc) {
        if (g_kerfurNpcClass) { bases[0] = g_kerfurNpcClass; nBases = 1; }
    } else if (g_kerfurPropClass) {
        bases[0] = g_kerfurPropClass; nBases = 1;
        if (g_floppyClass) { bases[1] = g_floppyClass; nBases = 2; }
    }
    if (nBases == 0) return;
    // The host's authoritative kerfurs of this form are wire mirrors, skipped; the only non-mirror
    // kerfur of the new form at the site is our own ghost.
    std::unordered_set<void*> mirrors;
    CollectMirrorActors(/*wantProp=*/!wantNpc, /*wantNpc=*/wantNpc, mirrors);
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;  // the new form spawns at the kerfur's own transform
    // The conversion verb spawns exactly one actor per base class at the conversion position, so
    // only the nearest untracked candidate per base class is claimed. Claiming everything within
    // the radius took the player's pre-existing save-loaded kerfur props, and the orphan reaper
    // then destroyed them; the join-time adoption makes those props mirrors (excluded above), and
    // the nearest-only rule covers a conversion racing that poll.
    void* bestObj[2] = {nullptr, nullptr};
    float bestD2[2]  = {kR2, kR2};
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (mirrors.count(obj)) continue;  // a host wire mirror -- not our ghost
        int b = -1;  // which base class this actor is (nearest tracked per base)
        for (size_t bi = 0; bi < nBases; ++bi)
            if (R::IsDescendantOfAny(cls, &bases[bi], 1)) { b = static_cast<int>(bi); break; }
        if (b < 0) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2[b]) { bestD2[b] = d2; bestObj[b] = obj; }
    }
    const auto now = std::chrono::steady_clock::now();
    int claimed = 0;
    for (size_t bi = 0; bi < nBases; ++bi) {
        void* obj = bestObj[bi];
        if (!obj) continue;
        if (wantNpc) {
            ue_wrap::puppet::DisableCharacterTicks(obj);  // park: the streamed pose becomes authoritative
            ue_wrap::kerfur::NeutralizeAiTimers(obj);      // stop the kerfur AI timers on the ghost
        }
        // A prop ghost is not frozen: the adopt finds it by eid wherever it settled, and a freeze
        // pinned it at the NPC's standing height for the host round-trip, a hang in the air then a
        // snap. It falls and settles by local physics, and the adopt rebinds it in place with a
        // sub-meter snap to the host's pose.
        g_parkedGhosts.push_back(ParkedGhost{obj, R::InternalIndexOf(obj),
                                             wantNpc ? uint8_t(0) : uint8_t(1), srcEid, now});
        ++claimed;
    }
    if (claimed)
        UE_LOGI("kerfur_convert[client]: claimed %d local conversion ghost(s) (%s) nearest-per-class near (%.0f,%.0f,%.0f) -- parked for host adoption",
                claimed, wantNpc ? "NPC turn-on" : "prop turn-off", x, y, z);
}

// Reap parked ghosts each poll: drop the ones that died or were adopted (now tracked by the
// host's entity), and destroy the ones the host never confirmed after the timeout (a rejected
// sentient kerfur, a dropped request), so they cannot be grabbed into a client-eid dupe. A
// cheap no-op when empty. Game thread.
void CleanupParkedGhosts() {
    if (g_parkedGhosts.empty()) return;
    bool haveProp = false, haveNpc = false;
    for (const auto& g : g_parkedGhosts) { if (g.toProp) haveProp = true; else haveNpc = true; }
    // Adopted means bound as a wire mirror; the per-actor reverse maps do not reflect that, so the
    // manager is asked directly.
    std::unordered_set<void*> mirrors;
    CollectMirrorActors(haveProp, haveNpc, mirrors);
    const auto now = std::chrono::steady_clock::now();
    constexpr long kOrphanTimeoutMs = 4000;  // host replies within ~RTT; 4 s un-adopted == orphan
    for (auto it = g_parkedGhosts.begin(); it != g_parkedGhosts.end();) {
        void* actor = it->actor;
        if (!actor || !R::IsLiveByIndex(actor, it->idx)) { it = g_parkedGhosts.erase(it); continue; }
        if (mirrors.count(actor)) { it = g_parkedGhosts.erase(it); continue; }  // adopted as a wire mirror -> success
        const long ageMs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->armed).count());
        if (ageMs >= kOrphanTimeoutMs) {
            UE_LOGI("kerfur_convert[client]: orphan conversion ghost actor=%p (%s) un-adopted after %ldms -- destroying (host rejected/dropped the toggle)",
                    actor, it->toProp ? "prop" : "NPC", ageMs);
            if (it->toProp) coop::prop_lifecycle::DestroyLocalProp(actor, /*deferred=*/false);
            else coop::npc_mirror::DestroyLocalNpcActor(actor);
            it = g_parkedGhosts.erase(it);
            continue;
        }
        ++it;
    }
}

void OnKerfurConvert(const coop::net::KerfurConvertBroadcastPayload& p, void* localPlayer) {
    auto* s = LoadSession();
    if (!s) return;
    // Client-only: the host converged inline (a silent register and the send), so it never applies
    // its own broadcast.
    if (s->role() != coop::net::Role::Client) return;
    const coop::element::ElementId oldEid = static_cast<coop::element::ElementId>(p.oldEid);
    const coop::element::ElementId newEid = static_cast<coop::element::ElementId>(p.newEid);
    const bool toNpc = (p.toForm == 0);  // toForm: 0 = NPC (turn-on), 1 = prop (turn_off)
    std::wstring classW;
    classW.reserve(p.newClassName.len);
    for (uint8_t i = 0; i < p.newClassName.len && i < 63; ++i)
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.newClassName.data[i])));

    if (p.rejected) {
        // The host refused (a sentient kerfur, a kill). If our old-form mirror still exists there
        // is nothing to do. If we converted optimistically (the poll detected our own conversion
        // and parked a ghost) the old mirror is gone: restore it at oldEid in the form the payload
        // names, the old one.
        if (coop::element::Registry::Get().Get(oldEid)) {
            UE_LOGI("kerfur_convert[client]: KerfurConvert rejected K=%u oldEid=%u -- mirror intact, no-op",
                    p.kerfurId, p.oldEid);
            return;
        }
        UE_LOGI("kerfur_convert[client]: KerfurConvert rejected K=%u oldEid=%u -- restoring local mirror (converted optimistically)",
                p.kerfurId, p.oldEid);
        // Restore the old form fresh, with no eid-keyed adopt: the parked ghost is the rejected new
        // form, the wrong one to adopt; the cleanup timeout reaps it.
        MaterializeKerfurMirror(toNpc, oldEid, coop::element::kInvalidId, classW, p.locX, p.locY, p.locZ,
                                p.rotPitch, p.rotYaw, p.rotRoll, localPlayer);
        return;
    }

    // Success: destroy the old-form mirror at oldEid, then materialise or adopt the new form at the
    // authoritative newEid.
    if (!toNpc) {
        // The new form is a prop, so the old form was the NPC.
        coop::net::EntityDestroyPayload dp{};
        dp.elementId = static_cast<uint32_t>(oldEid);
        coop::npc_mirror::OnEntityDestroy(dp);
    } else {
        // The new form is the NPC, so the old form was a prop: an eid-only teardown of the old prop
        // mirror. Its actor is evicted from the client held-pose map first, before the destroy
        // frees it; the map self-heals on read too.
        if (auto* oldEl = coop::element::Registry::Get().Get(oldEid))
            coop::kerfur_entity::ForgetKerfurPropMirror(oldEl->GetActor());
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        dp.elementId = static_cast<uint32_t>(oldEid);
        coop::remote_prop::OnDestroy(dp, localPlayer);
    }
    // Adopt the initiator's parked ghost by the converting eid, oldEid.
    MaterializeKerfurMirror(toNpc, newEid, /*adoptEid=*/oldEid, classW, p.locX, p.locY, p.locZ,
                            p.rotPitch, p.rotYaw, p.rotRoll, localPlayer);
    // The old-form retire for a join-window turn-on is not armed here: that broadcast cannot reach
    // a joiner mid save-transfer, so the NPC EntitySpawn receiver arms it and the quiescence sweep
    // runs it. A steady-state turn-on retired the old form through the destroy above.
    UE_LOGI("kerfur_convert[client]: applied KerfurConvert K=%u %s oldEid=%u -> newEid=%u class='%ls'",
            p.kerfurId, toNpc ? "->NPC(turn-on)" : "->prop(turn_off)", p.oldEid, p.newEid, classW.c_str());
}

// Take (find and remove) the parked ghost tagged with `srcEid` of the requested form, returning
// its actor or null. Header-declared, so defined outside the anonymous namespace; the parked
// list is still visible within this translation unit.
void* TakeParkedGhostByEid(uint32_t srcEid, bool wantNpc) {
    const uint8_t wantProp = wantNpc ? uint8_t(0) : uint8_t(1);
    for (auto it = g_parkedGhosts.begin(); it != g_parkedGhosts.end(); ++it) {
        if (it->srcEid != srcEid || it->toProp != wantProp) continue;
        void* actor = it->actor;
        const int32_t idx = it->idx;
        g_parkedGhosts.erase(it);
        return (actor && R::IsLiveByIndex(actor, idx)) ? actor : nullptr;
    }
    return nullptr;
}

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void SetClasses(void* npcClass, void* propClass, void* floppyClass) {
    g_kerfurNpcClass  = npcClass;
    g_kerfurPropClass = propClass;
    g_floppyClass     = floppyClass;
}

void OnDisconnect() {
    g_parkedGhosts.clear();  // game thread only
}

}  // namespace coop::kerfur_convert_client
