// coop/player/hand_record_sync.cpp -- see coop/player/hand_record_sync.h.

#include "coop/player/hand_record_sync.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::hand_record_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;
namespace N  = coop::net;

constexpr uint64_t kReadEveryMs = 250;  // the owner's hand, read at most this often

coop::net::Session* g_session = nullptr;
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 0;

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool Connected() { return g_session && g_session->running() && g_session->connected(); }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint8_t OwnSlot() { return coop::players::Registry::Get().LocalPeerId(); }

// One hand's record as the wire carries it: [u8 slot][the record].
struct Stored {
    bool have = false;
    std::vector<uint8_t> blob;
    SR::SaveRecord rec;
    ue_wrap::CachedObjRef appliedTo;  // the mirror it was last written into
    bool dirty = false;
};
Stored g_hand[N::kMaxPeers];

// The owner's side: what was last sent, and of which actor.
ue_wrap::CachedObjRef g_sentActor;
uint64_t g_sentHash = 0;
uint64_t g_nextReadMs = 0;

bool Decode(const std::vector<uint8_t>& blob, uint8_t& slot, SR::SaveRecord& rec) {
    size_t o = 0;
    return W::RdU8(blob, o, slot) && W::DeSave(blob, o, rec) && o == blob.size();
}

void Send(const std::vector<uint8_t>& blob, int toSlot) {
    if (toSlot >= 0) coop::blob_chunks::SendBlobToSlot(g_session, toSlot, N::ReliableKind::HandRecord, ++g_seq, blob);
    else coop::blob_chunks::SendBlob(g_session, N::ReliableKind::HandRecord, ++g_seq, blob);
}

// The owner's hand: its record read through the item's own getData and sent when the item or its record changed.
void TickOwner() {
    const uint64_t now = NowMs();
    if (now < g_nextReadMs) return;
    g_nextReadMs = now + kReadEveryMs;
    void* const hand = coop::hand_item::LocalHandActor();
    if (!hand || !R::IsLive(hand) || !SR::IsPropClass(R::ClassOf(hand)) || !SR::OverridesGetData(R::ClassOf(hand))) {
        g_sentActor.Set(nullptr);
        g_sentHash = 0;
        return;
    }
    SR::SaveRecord rec;
    if (!SR::CaptureRecord(hand, rec)) return;
    // Where the hand holds it moves every frame and is no part of what the mirror shows (the receiver keeps its own
    // base fields): it is left out, so only a change of the item itself sends.
    rec.xform = {};
    std::vector<uint8_t> blob;
    W::AppU8(blob, OwnSlot());
    W::SerSave(blob, rec);
    const uint64_t hash = coop::blob_chunks::Fnv64(blob);
    if (g_sentActor.Is(hand) && hash == g_sentHash) return;
    if (blob.size() > coop::blob_chunks::MaxBlobBytes()) {
        static bool s_said = false;
        if (!s_said) UE_LOGW("hand_record: the hand's %ls record is %zu B, over the blob ceiling -- not sent",
                             rec.className.c_str(), blob.size());
        s_said = true;
        return;
    }
    Send(blob, -1);
    g_sentActor.Set(hand);
    g_sentHash = hash;
    UE_LOGI("hand_record: this player's hand %ls sent (%zu B)", rec.className.c_str(), blob.size());
}

// Every stored record into its slot's current mirror: once per record, and again when the mirror is made anew.
void TickMirrors() {
    const uint8_t own = OwnSlot();
    const SR::ClassLoad load = IsHost() ? SR::ClassLoad::LookupOnly : SR::ClassLoad::Immediate;
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        Stored& h = g_hand[s];
        if (!h.have || s == own) continue;
        void* const mirror = coop::hand_item::MirrorActorForSlot(s);
        if (!mirror || !R::IsLive(mirror)) continue;
        if (!h.dirty && h.appliedTo.Is(mirror)) continue;
        if (R::ClassNameOf(mirror) != h.rec.className) continue;  // the hand moved on: its next record follows
        const bool ok = SR::ApplyRecord(mirror, h.rec, load);
        h.appliedTo.Set(mirror);
        h.dirty = false;
        UE_LOGI("hand_record: slot %u's hand %ls written into its mirror here (ok=%d)", s, h.rec.className.c_str(),
                ok ? 1 : 0);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session = session; }

void Tick() {
    if (!Connected()) return;
    TickOwner();
    TickMirrors();
    static uint64_t s_sweepMs = 0;
    if (!g_asm.Idle() && NowMs() >= s_sweepMs) {
        s_sweepMs = NowMs() + 1000;
        g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
    }
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    for (uint8_t s = 1; s < N::kMaxPeers; ++s)
        if (g_hand[s].have && s != slot) Send(g_hand[s].blob, slot);
    g_sentActor.Set(nullptr);  // the host's own hand goes again to everyone at its next read
}

void OnChunk(const coop::net::BlobChunkPayload& c, uint8_t senderSlot) {
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(c, senderSlot, blob)) return;
    uint8_t slot = 0;
    SR::SaveRecord rec;
    if (!Decode(blob, slot, rec) || slot >= N::kMaxPeers) {
        UE_LOGW("hand_record: a malformed hand record from slot %u -- dropped", senderSlot);
        return;
    }
    if (IsHost()) {
        // A client's own hand only, of the class its hand lane announced.
        if (senderSlot == 0 || slot != senderSlot || !coop::hand_item::HeldClassIs(slot, rec.className.c_str())) {
            UE_LOGW("hand_record: slot %u sent a hand record for slot %u of %ls, which its hand does not hold -- refused",
                    senderSlot, slot, rec.className.c_str());
            return;
        }
        Send(blob, -1);  // on to everyone; the sender skips its own slot
    } else if (senderSlot != 0) {
        return;  // a client takes hand records from the host alone
    }
    Stored& h = g_hand[slot];
    h.have = true;
    h.blob = std::move(blob);
    h.rec = std::move(rec);
    h.dirty = true;
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= N::kMaxPeers) return;
    g_hand[slot] = Stored{};
    g_asm.ClearSlot(slot);
}

void OnDisconnect() {
    for (Stored& h : g_hand) h = Stored{};
    g_asm.Clear();
    g_sentActor.Set(nullptr);
    g_sentHash = 0;
}

}  // namespace coop::hand_record_sync
