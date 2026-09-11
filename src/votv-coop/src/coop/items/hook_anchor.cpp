// coop/items/hook_anchor.cpp -- see the header for the transaction and what the arbiter takes.

#include "coop/items/hook_anchor.h"

#include "coop/element/intent_authority.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/keyed_objects.h"

#include "hook_sync_detail.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::hook_anchor {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace H  = ue_wrap::hook;
namespace W  = coop::save_record_wire;
namespace D  = coop::hook_sync::detail;

using SaveRecord = ue_wrap::save_record::SaveRecord;

// The record's own layout, measured off the game's getData: `names` slot 1 is [attachKey_A,
// attachKey_B, componentName_A, componentName_B] and `floats` slot 1 is [dist].
constexpr size_t kNameSlot  = 1;
constexpr size_t kFloatSlot = 1;

// The reach a hook anchor is allowed from the sender's body. The hook's own `maxDist` is the cable,
// not the throw, and the throw is a ballistic arc with `throwSpeed` behind it -- so this is
// generous on purpose and exists to catch an anchor on the other side of the map, not to police
// a long shot. Out of reach is logged, never refused.
constexpr float kAnchorReachUU = 6000.f;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler     g_asmCommit;
coop::blob_chunks::Assembler     g_asmAnchored;
uint32_t                         g_blobSeq = 0;

// What the HOST has adopted: the canonical hook, under the identity the handoff preserved.
struct Adopted {
    ue_wrap::CachedObjRef ref;
    uint8_t               ownerSlot = 0;
    uint16_t              seq       = 0;
};
std::vector<Adopted> g_adopted;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }

bool IsHost() {
    auto* s = Session();
    return s && s->role() == coop::net::Role::Host;
}

bool Finite(float v) { return std::isfinite(v); }
bool InWorld(float v) { return std::isfinite(v) && std::fabs(v) <= coop::net::kMaxCoord; }

bool SaneVec(const std::array<float, 3>& v) {
    return InWorld(v[0]) && InWorld(v[1]) && InWorld(v[2]);
}

// ---- the blob grammar --------------------------------------------------------------------------
// u16 seq | u8 classId | u8 ownerSlot | SaveRecord
void Serialize(std::vector<uint8_t>& b, uint16_t seq, uint8_t classId, uint8_t ownerSlot,
               const SaveRecord& r) {
    W::AppU8(b, static_cast<uint8_t>(seq & 0xFF));
    W::AppU8(b, static_cast<uint8_t>(seq >> 8));
    W::AppU8(b, classId);
    W::AppU8(b, ownerSlot);
    W::SerSave(b, r);
}

bool Parse(const std::vector<uint8_t>& b, uint16_t& seq, uint8_t& classId, uint8_t& ownerSlot,
           SaveRecord& r) {
    size_t o = 0;
    uint8_t lo = 0, hi = 0;
    if (!W::RdU8(b, o, lo) || !W::RdU8(b, o, hi)) return false;
    if (!W::RdU8(b, o, classId) || !W::RdU8(b, o, ownerSlot)) return false;
    seq = static_cast<uint16_t>(lo | (hi << 8));
    return W::DeSave(b, o, r);
}

std::wstring MintKey(uint8_t ownerSlot, uint16_t seq) {
    // Deterministic and namespaced, so a hook can never collide with a game-authored save key and
    // a reader of a save can see where the row came from.
    return L"mvhook_" + std::to_wstring(static_cast<unsigned>(ownerSlot)) + L"_" +
           std::to_wstring(static_cast<unsigned>(seq));
}

// Every float the record carries, bounds-checked. Strict on format: a record with a non-finite
// number in it is refused whole rather than clamped, because a clamp would invent a pose.
bool RecordSane(const SaveRecord& r) {
    for (float f : r.xform)
        if (!Finite(f)) return false;
    for (const auto& grp : r.vectors)
        for (const auto& v : grp)
            if (!SaneVec(v)) return false;
    for (const auto& grp : r.rotators)
        for (const auto& v : grp)
            if (!Finite(v[0]) || !Finite(v[1]) || !Finite(v[2])) return false;
    for (const auto& grp : r.floats)
        for (float f : grp)
            if (!Finite(f)) return false;
    return true;
}

// The arbiter's splice. Returns the record the host will actually apply.
SaveRecord Arbitrate(const SaveRecord& sent, uint8_t ownerSlot, uint16_t seq, H::Kind kind,
                     float maxDist) {
    SaveRecord out = sent;

    // IDENTITY IS THE HOST'S. A class name from the sender is a spawn primitive and a key from the
    // sender names an actor in the host's save.
    out.className = H::ClassNameOf(kind);
    out.key       = MintKey(ownerSlot, seq);

    // `dist` is a physics-constraint limit, and the game's own clamp on it is maxDist. Unclamped,
    // it is a number the sender chose for a constraint in the host's world.
    if (out.floats.size() > kFloatSlot && !out.floats[kFloatSlot].empty()) {
        float& d = out.floats[kFloatSlot][0];
        if (!std::isfinite(d) || d < 0.f) d = 0.f;
        if (maxDist > 0.f && d > maxDist) {
            UE_LOGI("hook_anchor: slot %u seq %u dist %.1f clamped to maxDist %.1f",
                    (unsigned)ownerSlot, (unsigned)seq, d, maxDist);
            d = maxDist;
        }
    }

    // The two attach keys ARE the anchor, and they are the only thing the handoff exists to move.
    // Resolve what we can and reach-check it; pass through what we cannot, because our index is
    // not the game's registry and a refusal here would break a legitimate anchor.
    if (out.names.size() > kNameSlot) {
        auto* s = Session();
        const bool clientSender = s && ownerSlot != 0;
        for (size_t i = 0; i < 2 && i < out.names[kNameSlot].size(); ++i) {
            const std::wstring& k = out.names[kNameSlot][i];
            if (k.empty() || k == L"None") continue;
            void* anchor = coop::prop_element_tracker::FindLiveActorByKey(k);
            if (!anchor) anchor = ue_wrap::keyed_objects::Resolve(k.c_str());
            if (!anchor) continue;  // not ours to judge; the game resolves it or does not
            if (!clientSender) continue;  // the host's own anchors: it may cheat, by definition
            const auto tok = coop::element::IntentTarget::ForClientIntent(*s, ownerSlot,
                                                                         kAnchorReachUU);
            const coop::element::IntentSubject sub = tok.Authorize(anchor);
            if (!sub) {
                UE_LOGW("hook_anchor: slot %u seq %u anchor %c key '%ls' is %s (reach %.0f, "
                        "measured %.0f) -- APPLIED anyway; bounds are client-scoped and this costs "
                        "trust, not display",
                        (unsigned)ownerSlot, (unsigned)seq, i == 0 ? 'A' : 'B', k.c_str(),
                        coop::element::OutcomeName(sub.outcome), sub.reachUU, sub.distUU);
            }
        }
    }
    return out;
}

void BroadcastAnchored(uint8_t ownerSlot, uint16_t seq, H::Kind kind, const SaveRecord& r) {
    auto* s = Session();
    if (!s) return;
    std::vector<uint8_t> blob;
    Serialize(blob, seq, static_cast<uint8_t>(kind), ownerSlot, r);
    if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::HookAnchored, ++g_blobSeq, blob)) {
        UE_LOGW("hook_anchor: HookAnchored blob for slot %u seq %u did NOT send -- every peer keeps "
                "an owner-phase mirror of a hook that is now the host's",
                (unsigned)ownerSlot, (unsigned)seq);
    }
}

void SendDestroyFor(uint8_t ownerSlot, uint16_t seq) {
    auto* s = Session();
    if (!s) return;
    coop::net::HookDestroyPayload d{};
    d.seq        = seq;
    d.originSlot = ownerSlot;
    s->SendReliable(coop::net::ReliableKind::HookDestroy, &d, sizeof(d));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool SendCommit(uint16_t seq, H::Kind kind, void* hookActor, const H::State& /*st*/) {
    auto* s = Session();
    if (!s || !hookActor) return false;
    SaveRecord rec;
    if (!H::CaptureRecord(hookActor, rec)) {
        UE_LOGW("hook_anchor: getData refused on seq %u -- the anchor cannot be handed over and the "
                "hook stays this peer's; it will be retried on the next poll", (unsigned)seq);
        return false;
    }
    std::vector<uint8_t> blob;
    Serialize(blob, seq, static_cast<uint8_t>(kind), 0, rec);
    if (blob.size() > coop::blob_chunks::MaxBlobBytes()) {
        UE_LOGE("hook_anchor: commit blob for seq %u is %zu bytes, past the transport ceiling",
                (unsigned)seq, blob.size());
        return false;
    }
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::HookAnchorCommit, ++g_blobSeq,
                                       blob);
}

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread() || !IsHost()) return;
    std::vector<uint8_t> blob;
    if (!g_asmCommit.OnChunk(p, senderSlot, blob)) return;

    uint16_t seq = 0; uint8_t classId = 0, unused = 0;
    SaveRecord rec;
    if (!Parse(blob, seq, classId, unused, rec)) {
        UE_LOGW("hook_anchor: malformed commit from slot %u -- dropped", (unsigned)senderSlot);
        return;
    }
    if (seq == 0 || classId >= static_cast<uint8_t>(H::Kind::Count) || !RecordSane(rec)) {
        UE_LOGW("hook_anchor: commit from slot %u refused (seq=%u classId=%u sane=%d)",
                (unsigned)senderSlot, (unsigned)seq, (unsigned)classId, RecordSane(rec) ? 1 : 0);
        // Positive host-authoritative evidence, never silence: the owner's local hook would
        // otherwise stand there forever as a hook nobody else has.
        SendDestroyFor(senderSlot, seq);
        return;
    }
    const H::Kind kind = static_cast<H::Kind>(classId);

    const ue_wrap::FVector  loc{rec.xform[4], rec.xform[5], rec.xform[6]};
    const ue_wrap::FRotator rot{};
    void* actor = H::SpawnCanonical(kind, loc, rot);
    if (!actor) {
        UE_LOGW("hook_anchor: could not spawn the canonical hook for slot %u seq %u",
                (unsigned)senderSlot, (unsigned)seq);
        SendDestroyFor(senderSlot, seq);
        return;
    }
    const SaveRecord applied = Arbitrate(rec, senderSlot, seq, kind, H::MaxDistOf(actor));
    if (!H::AdoptRecord(actor, applied)) {
        UE_LOGW("hook_anchor: loadData refused on the canonical hook for slot %u seq %u",
                (unsigned)senderSlot, (unsigned)seq);
        E::DestroyActor(actor);
        SendDestroyFor(senderSlot, seq);
        return;
    }
    // THE RECORD DOES NOT CARRY THE PHASE. Without this the host's own hook fails unhook_'s
    // attached_a && attached_b gate and is destroyed instead of dropping the pickup -- the player
    // could never get the hook back.
    H::WriteAttachedFlags(actor, true, true);

    Adopted a;
    a.ref.Set(actor);
    a.ownerSlot = senderSlot;
    a.seq       = seq;
    g_adopted.push_back(a);

    UE_LOGI("hook_anchor: ADOPTED slot %u seq %u as key '%ls' (actor=%p) -- the host owns it now",
            (unsigned)senderSlot, (unsigned)seq, applied.key.c_str(), actor);
    BroadcastAnchored(senderSlot, seq, kind, applied);
}

void OnAnchoredChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread()) return;
    std::vector<uint8_t> blob;
    if (!g_asmAnchored.OnChunk(p, senderSlot, blob)) return;
    if (IsHost()) return;  // the host built it; this is its own broadcast coming back

    uint16_t seq = 0; uint8_t classId = 0, ownerSlot = 0;
    SaveRecord rec;
    if (!Parse(blob, seq, classId, ownerSlot, rec)) return;
    if (seq == 0 || classId >= static_cast<uint8_t>(H::Kind::Count) || !RecordSane(rec)) return;
    if (ownerSlot >= coop::players::kMaxPeers) return;
    if (!H::EnsureResolved()) return;

    const D::Key key = D::MakeKey(ownerSlot, seq);

    // The owner's own real hook, and every peer's owner-phase mirror, go in the same statement that
    // builds the anchored one -- so there is never a frame with two, or a frame with none.
    if (ownerSlot == coop::players::Registry::Get().LocalPeerId()) {
        for (size_t i = 0; i < D::OwnedHooks().size(); ++i) {
            if (D::OwnedHooks()[i].seq != seq) continue;
            if (void* mine = D::OwnedHooks()[i].ref.Get()) E::DestroyActor(mine);
            D::OwnedHooks().erase(D::OwnedHooks().begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    D::DropMirror(key);

    // The joiner's dedup. A hook the joiner booted out of the host's save is registered in the
    // game's own key map by the LOAD path; one the host adopted after that capture is not. So a
    // resolvable key means "the save already gave me this" and the replay is a no-op.
    if (!rec.key.empty() && ue_wrap::keyed_objects::Resolve(rec.key.c_str())) {
        UE_LOGI("hook_anchor: anchored hook '%ls' is already in my world from the save -- replay "
                "skipped", rec.key.c_str());
        return;
    }

    const ue_wrap::FVector  loc{rec.xform[4], rec.xform[5], rec.xform[6]};
    const ue_wrap::FRotator rot{};
    void* m = D::InstallMirror(key, static_cast<H::Kind>(classId), loc, rot, /*anchored=*/true);
    if (!m) return;
    // The mirror re-derives its own pose: loadData + processKeys resolve both attach keys in THIS
    // world and re-attach, so it rides this peer's copy of whatever the original was tied to and
    // never needs a pose on the wire. The two attached flags are deliberately NOT restored here --
    // see ue_wrap/actors/hook.h.
    H::AdoptRecord(m, rec);
    E::SetActorEnableCollision(m, false);  // loadData re-enables both of the hook's collision spheres
}

void TickHost(uint64_t /*nowMs*/) {
    if (!IsHost()) return;
    for (size_t i = 0; i < g_adopted.size();) {
        if (g_adopted[i].ref.Alive()) { ++i; continue; }
        UE_LOGI("hook_anchor: adopted hook slot %u seq %u died -- announcing",
                (unsigned)g_adopted[i].ownerSlot, (unsigned)g_adopted[i].seq);
        SendDestroyFor(g_adopted[i].ownerSlot, g_adopted[i].seq);
        g_adopted.erase(g_adopted.begin() + static_cast<ptrdiff_t>(i));
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (!GT::IsGameThread() || !IsHost() || peerSlot <= 0) return;
    auto* s = Session();
    if (!s) return;
    size_t sent = 0;
    for (const auto& a : g_adopted) {
        void* actor = a.ref.Get();
        if (!actor) continue;
        SaveRecord rec;
        if (!H::CaptureRecord(actor, rec)) continue;
        std::vector<uint8_t> blob;
        Serialize(blob, a.seq, static_cast<uint8_t>(H::KindOf(actor)), a.ownerSlot, rec);
        if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::HookAnchored,
                                              ++g_blobSeq, blob)) {
            ++sent;
        }
    }
    if (sent) UE_LOGI("hook_anchor: replayed %zu anchored hooks to slot %d", sent, peerSlot);
}

void OnDisconnect() {
    g_adopted.clear();
    g_asmCommit.Clear();
    g_asmAnchored.Clear();
    ue_wrap::keyed_objects::ResetCache();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hook_anchor
