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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace coop::hook_anchor {
namespace {

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

// The reach a hook anchor is allowed from the sender's body. The hook's own maxDist is the cable,
// not the throw, and the throw is a ballistic arc -- so this is generous on purpose and exists to
// catch an anchor on the other side of the map, not to police a long shot. Logged, never refused.
constexpr float kAnchorReachUU = 6000.f;

// A bound on what one sender can make the host build. Every adoption is a save-participating actor
// that ticks and persists, so an unbounded table is an unbounded world.
constexpr size_t kMaxAdoptedPerSlot = 16;

// The assembler sweep the blob facility asks every consumer for. A half-blob that never completes
// otherwise holds its reservation for the whole session.
constexpr uint64_t kSweepEveryMs = 1000;
constexpr auto     kAssemblyTtl  = std::chrono::seconds(20);

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler     g_asmCommit;
coop::blob_chunks::Assembler     g_asmAnchored;
uint32_t                         g_blobSeq   = 0;
uint64_t                         g_lastSweep = 0;
uint32_t                         g_keyNonce  = 0;

// What the HOST has adopted: the canonical hook, under the identity the handoff preserved. The row
// type is the lane's, in its detail header, because the prop-claim feeder reads these rows too.
using Adopted = D::Adopted;
std::vector<Adopted> g_adopted;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }

bool IsHost() {
    auto* s = Session();
    return s && s->role() == coop::net::Role::Host;
}

bool InWorld(float v) { return std::isfinite(v) && std::fabs(v) <= coop::net::kMaxCoord; }
bool SaneVec(const std::array<float, 3>& v) { return InWorld(v[0]) && InWorld(v[1]) && InWorld(v[2]); }

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

// The minted key has to be unique against the SAVE, not just against this session. The sequence
// restarts at zero every launch while the key it goes into persists forever, so `slot_seq` alone
// would hand session two the names session one's hooks already hold in the loaded save -- and a
// joiner's dedup would then resolve the OLD hook and skip the new one for good. The per-session
// nonce is what keeps the two apart.
std::wstring MintKey(uint8_t ownerSlot, uint16_t seq) {
    wchar_t buf[48];
    swprintf(buf, 48, L"mvhook_%08x_%u_%u", g_keyNonce, static_cast<unsigned>(ownerSlot),
             static_cast<unsigned>(seq));
    return std::wstring(buf);
}

// Every number the record carries, bounds-checked against the world. Strict on format: a record
// with an out-of-world number in it is refused whole rather than clamped, because a clamp would
// invent a pose. `xform` is checked with the same bound as the rest -- it is the spawn location on
// the host and on every receiver, so leaving it merely finite would let a sender place a
// save-participating actor at 1e38.
bool RecordSane(const SaveRecord& r) {
    for (size_t i = 0; i < r.xform.size(); ++i) {
        if (!std::isfinite(r.xform[i])) return false;
        if (i >= 4 && i <= 6 && !InWorld(r.xform[i])) return false;   // translation
    }
    for (const auto& grp : r.vectors)
        for (const auto& v : grp)
            if (!SaneVec(v)) return false;
    for (const auto& grp : r.rotators)
        for (const auto& v : grp)
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
    for (const auto& grp : r.floats)
        for (float f : grp)
            if (!std::isfinite(f)) return false;
    return true;
}

size_t AdoptedForSlot(uint8_t slot) {
    size_t n = 0;
    for (const auto& a : g_adopted)
        if (a.ownerSlot == slot) ++n;
    return n;
}

bool AlreadyAdopted(uint8_t slot, uint16_t seq) {
    for (const auto& a : g_adopted)
        if (a.ownerSlot == slot && a.seq == seq) return true;
    return false;
}

// The arbiter's splice. Returns the record the host will actually apply.
SaveRecord Arbitrate(const SaveRecord& sent, uint8_t ownerSlot, uint16_t seq, H::Kind kind,
                     float maxDist) {
    SaveRecord out = sent;

    // IDENTITY IS THE HOST'S. A class name from the sender is a spawn primitive, and a key from the
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
    // Resolve what we can and reach-check it; pass through what we cannot, because our index is not
    // the game's registry and a refusal here would break a legitimate anchor.
    if (out.names.size() > kNameSlot) {
        auto* s = Session();
        const bool clientSender = s && ownerSlot != 0;
        for (size_t i = 0; i < 2 && i < out.names[kNameSlot].size(); ++i) {
            const std::wstring& k = out.names[kNameSlot][i];
            if (k.empty() || k == L"None") continue;
            void* anchor = coop::prop_element_tracker::FindLiveActorByKey(k);
            if (!anchor) anchor = ue_wrap::keyed_objects::Resolve(k.c_str());
            if (!anchor) continue;          // not ours to judge; the game resolves it or does not
            if (!clientSender) continue;    // the host's own anchors: it may cheat, by definition
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

void SendDestroyFor(uint8_t ownerSlot, uint16_t seq, bool anchored) {
    auto* s = Session();
    if (!s) return;
    coop::net::HookDestroyPayload d{};
    d.seq        = seq;
    d.originSlot = ownerSlot;
    d.anchored   = anchored ? 1 : 0;
    s->SendReliable(coop::net::ReliableKind::HookDestroy, &d, sizeof(d));
}

// The host's side of an adoption, shared by a client's commit and the host's own anchor. `actor` is
// the hook that will be canonical from here on: for a client it is the one the host just spawned,
// for the host it is the real hook the player is standing next to.
void RegisterAdopted(void* actor, uint8_t ownerSlot, uint16_t seq) {
    Adopted a;
    a.ref.Set(actor);
    a.ownerSlot = ownerSlot;
    a.seq       = seq;
    g_adopted.push_back(a);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_keyNonce == 0) {
        std::random_device rd;
        g_keyNonce = (static_cast<uint32_t>(rd()) | 1u);
    }
}

CommitResult SendCommit(uint16_t seq, H::Kind kind, void* hookActor) {
    auto* s = Session();
    if (!s || !hookActor) return CommitResult::Failed;

    SaveRecord rec;
    if (!H::CaptureRecord(hookActor, rec)) {
        UE_LOGW("hook_anchor: getData refused on seq %u -- the anchor cannot be handed over and the "
                "hook stays this peer's; it is retried on the next poll", (unsigned)seq);
        return CommitResult::Failed;
    }

    // THE HOST IS THE ARBITER, so it does not send itself a question. SendReliable fans out to the
    // peers and never loops back, and the receiver is host-gated, so a host that shipped its own
    // commit would be talking to nobody: the hook would never be adopted, never be broadcast, and
    // every client would keep a frozen owner-phase mirror welded to the host's body forever.
    if (IsHost()) {
        if (AlreadyAdopted(0, seq)) return CommitResult::AdoptedLocally;
        const SaveRecord applied = Arbitrate(rec, 0, seq, kind, H::MaxDistOf(hookActor));
        // The host's hook IS the canonical one already -- it is not respawned, only named. The
        // record carries the minted key into it, and the two attached flags are re-asserted for
        // the same reason a client's adoption needs them.
        if (!H::AdoptRecord(hookActor, applied)) {
            UE_LOGW("hook_anchor: loadData refused on the host's own hook seq %u", (unsigned)seq);
            return CommitResult::Failed;
        }
        H::WriteAttachedFlags(hookActor, true, true);
        RegisterAdopted(hookActor, 0, seq);
        UE_LOGI("hook_anchor: ADOPTED the host's own hook seq=%u as key '%ls'", (unsigned)seq,
                applied.key.c_str());
        BroadcastAnchored(0, seq, kind, applied);
        return CommitResult::AdoptedLocally;
    }

    std::vector<uint8_t> blob;
    Serialize(blob, seq, static_cast<uint8_t>(kind), 0, rec);
    if (blob.size() > coop::blob_chunks::MaxBlobBytes()) {
        UE_LOGE("hook_anchor: commit blob for seq %u is %zu bytes, past the transport ceiling",
                (unsigned)seq, blob.size());
        return CommitResult::Failed;
    }
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::HookAnchorCommit, ++g_blobSeq,
                                       blob)
               ? CommitResult::Sent
               : CommitResult::Failed;
}

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread() || !IsHost()) return;
    if (senderSlot == 0 || senderSlot >= coop::players::kMaxPeers) return;  // clients only
    std::vector<uint8_t> blob;
    if (!g_asmCommit.OnChunk(p, senderSlot, blob)) return;

    uint16_t seq = 0; uint8_t classId = 0, unused = 0;
    SaveRecord rec;
    if (!Parse(blob, seq, classId, unused, rec)) {
        UE_LOGW("hook_anchor: malformed commit from slot %u -- dropped", (unsigned)senderSlot);
        return;
    }
    if (seq == 0 || classId >= static_cast<uint8_t>(H::Kind::Count) || !RecordSane(rec)) {
        UE_LOGW("hook_anchor: commit from slot %u refused (seq=%u classId=%u)",
                (unsigned)senderSlot, (unsigned)seq, (unsigned)classId);
        // Positive host-authoritative evidence, never silence: the owner's local hook is retired
        // with it, so nobody is left holding a hook the others cannot see.
        SendDestroyFor(senderSlot, seq, /*anchored=*/false);
        return;
    }
    // A second commit for a pair already adopted would spawn a SECOND save-participating actor
    // under the SAME minted key. The first adoption stands and the repeat is simply not a question.
    if (AlreadyAdopted(senderSlot, seq)) {
        UE_LOGW("hook_anchor: slot %u seq %u is already adopted -- repeat commit ignored",
                (unsigned)senderSlot, (unsigned)seq);
        return;
    }
    if (AdoptedForSlot(senderSlot) >= kMaxAdoptedPerSlot) {
        UE_LOGW("hook_anchor: slot %u is at the %zu-adoption ceiling -- refusing. Each adoption is "
                "an actor that ticks and persists, so this is a bound on the world, not a rate.",
                (unsigned)senderSlot, kMaxAdoptedPerSlot);
        SendDestroyFor(senderSlot, seq, /*anchored=*/false);
        return;
    }
    const H::Kind kind = static_cast<H::Kind>(classId);

    const ue_wrap::FVector  loc{rec.xform[4], rec.xform[5], rec.xform[6]};
    const ue_wrap::FRotator rot{};
    void* actor = H::SpawnCanonical(kind, loc, rot);
    if (!actor) {
        UE_LOGW("hook_anchor: could not spawn the canonical hook for slot %u seq %u",
                (unsigned)senderSlot, (unsigned)seq);
        SendDestroyFor(senderSlot, seq, /*anchored=*/false);
        return;
    }
    const SaveRecord applied = Arbitrate(rec, senderSlot, seq, kind, H::MaxDistOf(actor));
    if (!H::AdoptRecord(actor, applied)) {
        UE_LOGW("hook_anchor: loadData refused on the canonical hook for slot %u seq %u",
                (unsigned)senderSlot, (unsigned)seq);
        E::DestroyActor(actor);
        SendDestroyFor(senderSlot, seq, /*anchored=*/false);
        return;
    }
    // THE RECORD DOES NOT CARRY THE PHASE. Without this the host's own hook fails unhook_'s
    // attached_a && attached_b gate and is destroyed instead of dropping the pickup -- the player
    // could never get the hook back.
    H::WriteAttachedFlags(actor, true, true);

    // The host holds an owner-phase mirror of this hook too: it processes a client's HookState
    // locally, like any other receiver. It goes in the same breath as the adoption, or the host
    // keeps a frozen copy welded to that client's body for the rest of the session.
    D::DropMirror(D::MakeKey(senderSlot, seq, /*anchored=*/false));

    RegisterAdopted(actor, senderSlot, seq);
    UE_LOGI("hook_anchor: ADOPTED slot %u seq %u as key '%ls' (actor=%p) -- the host owns it now",
            (unsigned)senderSlot, (unsigned)seq, applied.key.c_str(), actor);
    BroadcastAnchored(senderSlot, seq, kind, applied);
}

void OnAnchoredChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread()) return;
    // The host built it; its own broadcast never comes back, and a client forging this kind must
    // not be able to occupy an assembly slot on the host either -- so the role gate comes BEFORE
    // the assembler, not after it.
    if (IsHost()) return;
    if (senderSlot != 0) return;  // host-authored only

    std::vector<uint8_t> blob;
    if (!g_asmAnchored.OnChunk(p, senderSlot, blob)) return;

    uint16_t seq = 0; uint8_t classId = 0, ownerSlot = 0;
    SaveRecord rec;
    if (!Parse(blob, seq, classId, ownerSlot, rec)) return;
    if (seq == 0 || classId >= static_cast<uint8_t>(H::Kind::Count) || !RecordSane(rec)) return;
    if (ownerSlot >= coop::players::kMaxPeers) return;
    if (!H::EnsureResolved()) return;

    // The owner's own real hook, and every peer's owner-phase mirror, go in the same statement that
    // builds the anchored one -- so there is never a frame with two, or a frame with none.
    if (ownerSlot == coop::players::Registry::Get().LocalPeerId()) {
        D::DropOwnedBySeq(seq);
    }
    D::DropMirror(D::MakeKey(ownerSlot, seq, /*anchored=*/false));

    // The joiner's dedup. A hook the joiner booted out of the host's save is registered in the
    // game's own key map by the LOAD path; one the host adopted after that capture is not. So a
    // resolvable key means "the save already gave me this" and the replay is a no-op.
    //
    // It FAILS CLOSED. If the resolver itself is unavailable we cannot tell the two apart, and the
    // save is the primary channel for anchored hooks -- so the replay is skipped rather than
    // risking a second hook on top of the one the save loaded.
    if (!rec.key.empty()) {
        if (!ue_wrap::keyed_objects::Available()) {
            UE_LOGW("hook_anchor: the game's key resolver is unavailable, so '%ls' cannot be "
                    "checked against what the save already loaded -- skipping the replay rather "
                    "than risking a duplicate", rec.key.c_str());
            return;
        }
        if (ue_wrap::keyed_objects::Resolve(rec.key.c_str())) return;  // the save has it already
    }

    const ue_wrap::FVector  loc{rec.xform[4], rec.xform[5], rec.xform[6]};
    const ue_wrap::FRotator rot{};
    void* m = D::InstallMirror(D::MakeKey(ownerSlot, seq, /*anchored=*/true),
                               static_cast<H::Kind>(classId), loc, rot, /*anchored=*/true);
    if (!m) return;
    // The mirror re-derives its own pose: loadData + processKeys resolve both attach keys in THIS
    // world and re-attach, so it rides this peer's copy of whatever the original was tied to and
    // never needs a pose on the wire. The two attached flags are deliberately NOT restored here --
    // see ue_wrap/actors/hook.h.
    H::AdoptRecord(m, rec);
    E::SetActorEnableCollision(m, false);  // loadData re-enables both of the hook's spheres
}

void TickHost(uint64_t nowMs) {
    // The blob facility asks every consumer for a periodic sweep; a half-assembly that never
    // completes otherwise holds its reservation for the whole session, and a sender can exhaust
    // its own allowance deliberately.
    if (nowMs - g_lastSweep >= kSweepEveryMs) {
        g_lastSweep = nowMs;
        const auto now = std::chrono::steady_clock::now();
        g_asmCommit.Sweep(now, kAssemblyTtl);
        g_asmAnchored.Sweep(now, kAssemblyTtl);
    }
    if (!IsHost()) return;
    for (size_t i = 0; i < g_adopted.size();) {
        if (g_adopted[i].ref.Alive()) { ++i; continue; }
        UE_LOGI("hook_anchor: adopted hook slot %u seq %u died -- announcing",
                (unsigned)g_adopted[i].ownerSlot, (unsigned)g_adopted[i].seq);
        SendDestroyFor(g_adopted[i].ownerSlot, g_adopted[i].seq, /*anchored=*/true);
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

void OnPeerLeftSlot(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    // Slots recycle lowest-free, so a rejoining peer's chunks must never merge into the previous
    // occupant's half-assembly under the same id.
    g_asmCommit.ClearSlot(static_cast<uint8_t>(slot));
    g_asmAnchored.ClearSlot(static_cast<uint8_t>(slot));
    // The adopted rows STAY: the host owns those hooks, and they are in its save.
}

void OnDisconnect() {
    g_adopted.clear();
    g_asmCommit.Clear();
    g_asmAnchored.Clear();
    ue_wrap::keyed_objects::ResetCache();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hook_anchor

namespace coop::hook_sync::detail {
std::vector<Adopted>& AdoptedHooks() { return coop::hook_anchor::g_adopted; }
}  // namespace coop::hook_sync::detail
