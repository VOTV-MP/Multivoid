// coop/interactables/laptop_sync.cpp -- see coop/interactables/laptop_sync.h.

#include "coop/interactables/laptop_sync.h"

#include "coop/comms/chat_feed.h"  // ToUtf8 (shared converter)
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/interactables/laptop_buffer_sync.h"  // the quad baseline prime
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/portable_pc.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace coop::laptop_sync {
namespace {

namespace L = ue_wrap::laptop;
namespace PPC = ue_wrap::portable_pc;
namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kPollMs        = 250;    // the 4 Hz edge poll
constexpr uint64_t kLidSweepMs    = 1000;   // 1 Hz portable-PC lid sweep (the rack cadence)
constexpr uint64_t kChunkTtlMs    = 10000;  // half-assembled content stream TTL
constexpr uint64_t kPendingTtlMs  = 30000;  // deferred lid apply TTL
constexpr size_t   kContentCapBytes = 4096;  // the total content cap (truncate with a warning)
// The pending-lid table is keyed by a wire eid and inserted into precisely when that eid does
// not resolve: the garbage case is the inserting case, so an attacker-chosen eid stream grows
// it at line rate, and the TTL bounds it in time but not in rate. This is the absolute size
// bound. Refuse a new key rather than evict the oldest: eviction lets a newcomer displace an
// incumbent, so a flood would push out exactly the legitimate birth-lane-skew entry the map
// exists to hold; refusing degrades the flooder. Sizing: legitimate entries are bounded by the
// portable PCs whose eid has not yet resolved on this peer, single digits, so 64 is generous
// and the map stays under a couple of kilobytes. The residual: under a sustained flood a
// legitimate lid op can be refused while the table is full; not lost permanently, since the
// 4 Hz edge poll on the authoring side re-detects the unchanged lid and re-sends, but there
// is a window.
constexpr size_t   kLidPendingCap = 64;

// The content-blob head: a kind byte and an eid the slot lane does not use (kind 0, eid 0). The
// disc's own content left this lane with kind 1: a disc's state is its SAVE RECORD, addressed by
// Key, and coop/props/prop_save_data carries it -- an eid names an actor, and this content's whole
// problem was that the actor is destroyed and remade.
constexpr size_t kBlobHead = 5;

uint64_t NowMs() {
    return static_cast<uint64_t>(::GetTickCount64());
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The poll baselines, primed on every wire apply (the apply-and-prime shape).
bool    g_havePrev = false;
bool    g_prevOpened = false;
int32_t g_prevType = -1;
uint64_t g_nextPoll = 0;

// Wire-target power convergence: a pending target consumes the matching local edge as
// wire-transient (a state predicate, not a flag timer); a non-matching edge is organic and
// broadcasts.
bool g_wantValid = false;
bool g_wantOpened = false;

// The post-eject disc-content publish watch (both roles; the client sends to the host, the
// host broadcasts).

// The content-blob reassembly (keyed by sender and sequence) and the per-sender sequence mint
// shared by the broadcast and to-slot sends: one counter per kind owner, so the pair is
// unique at every receiver.
coop::blob_chunks::Assembler g_blobAsm;
uint32_t g_blobSeq = 1;

// The lid axis (the portable PC, op 6): a per-eid previous map (first sight primes silently)
// and the unresolvable-eid stash (the spawn rides another kind; the skew is under the TTL).
std::map<uint32_t, bool> g_lidPrev;
struct PendingLid { bool opened; uint64_t deadline; };
std::map<uint32_t, PendingLid> g_lidPending;
uint64_t g_nextLidSweep = 0;

// The occupied-slot scalar edge is not applied on arrival: it parks here until its content
// stream assembles, and the scalars and strings land in one write (applying scalars with
// empty strings first opened a window where a local eject on the receiver spawned a
// content-less disc that became the canonical cross-peer prop). In-lane ordering guarantees
// the chunks follow their edge; the TTL is the lost-stream fallback (a degraded scalar-only
// apply with a warning).
struct PendingSlot {
    bool valid = false;
    uint8_t sender = 0xFF;
    L::SlotState st;
    uint64_t deadline = 0;
};
// The park is per sender: two peers' concurrent inserts (both portals view the one laptop,
// and each's occupied guard runs against its own possibly stale mirror) must never
// cross-pair scalars with the other's content stream. Keyed by the sender slot; the consume
// matches the content blob's origin exactly.
std::map<uint8_t, PendingSlot> g_pendingSlots;

bool g_announced = false;

// Serialisation: fields joined by 0x1F, UTF-8.
constexpr char kSep = '\x1F';

std::string PackSlotContent(const L::SlotContent& c) {
    std::string out = coop::chat_feed::ToUtf8(c.nametype);
    out += kSep;
    out += coop::chat_feed::ToUtf8(c.objectData);
    for (const auto& d : c.data) { out += kSep; out += coop::chat_feed::ToUtf8(d); }
    return out;
}

L::SlotContent UnpackSlotContent(const std::string& bytes) {
    L::SlotContent c;
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= bytes.size(); ++i) {
        if (i == bytes.size() || bytes[i] == kSep) {
            parts.push_back(bytes.substr(start, i - start));
            start = i + 1;
        }
    }
    if (!parts.empty()) c.nametype = FromUtf8(parts[0]);
    if (parts.size() > 1) c.objectData = FromUtf8(parts[1]);
    for (size_t i = 2; i < parts.size(); ++i) c.data.push_back(FromUtf8(parts[i]));
    return c;
}

// The send helpers.
void SendOut(coop::net::Session* s, const coop::net::LaptopStatePayload& p, int exceptSlot) {
    if (s->role() == coop::net::Role::Client) {
        s->SendReliableToSlot(0, coop::net::ReliableKind::LaptopState, &p, sizeof(p));
        return;
    }
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (slot == exceptSlot || !s->IsSlotReady(slot)) continue;
        s->SendReliableToSlot(slot, coop::net::ReliableKind::LaptopState, &p, sizeof(p),
                              exceptSlot > 0 ? static_cast<uint8_t>(exceptSlot) : 0);
    }
}

// Build the content-blob bytes (kind, eid, content) with the cap warning.
std::vector<uint8_t> MakeContentBlob(uint8_t kind, uint32_t eid, const std::string& bytes) {
    std::string data = bytes;
    if (data.size() > kContentCapBytes) {
        UE_LOGW("laptop_sync: content (kind=%u eid=%u) %zu B over the %zu cap -- TRUNCATED "
                "(OPEN-9 residual)", kind, eid, data.size(), kContentCapBytes);
        data.resize(kContentCapBytes);
    }
    std::vector<uint8_t> blob(kBlobHead + data.size());
    blob[0] = kind;
    std::memcpy(blob.data() + 1, &eid, 4);
    if (!data.empty()) std::memcpy(blob.data() + kBlobHead, data.data(), data.size());
    return blob;
}

// Broadcast content (the host to every ready slot; the client to the host, which re-fans
// each chunk unchanged with the origin byte).
void SendContentBlob(coop::net::Session* s, uint8_t kind, uint32_t eid,
                     const std::string& bytes) {
    coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::LaptopBlob,
                                g_blobSeq++, MakeContentBlob(kind, eid, bytes));
}

void PrimeBaselines() {
    L::PowerState ps;
    L::SlotState st;
    if (L::ReadPower(ps) && L::ReadSlot(st)) {
        g_prevOpened = ps.isOpened;
        g_prevType = st.floppyType;
        g_havePrev = true;
    }
    // The invariant: every wire-driven laptop-state write path terminates here, so the quad
    // shadow primes at the same single point and can never read a wire apply as an organic edit.
    coop::laptop_buffer_sync::PrimeQuadBaseline();
}

// The local slot-content publish for an insert edge (organic; a replay onto us never
// happens, since receivers write raw, primed). Reads the laptop's live scalars and strings
// and ships the edge plus the chunks.
void BroadcastInsert(coop::net::Session* s) {
    L::SlotState st;
    L::SlotContent c;
    if (!L::ReadSlot(st) || !L::ReadSlotContent(c)) return;
    coop::net::LaptopStatePayload p{};
    p.op = 1;
    p.zip = st.zip ? 1 : 0;
    p.floppyType = st.floppyType;
    p.readWrites = st.readWrites;
    SendOut(s, p, -1);
    SendContentBlob(s, /*kind*/0, /*eid*/0, PackSlotContent(c));
    UE_LOGI("laptop_sync: local INSERT edge (type=%d zip=%u rw=%d) -- broadcast + content",
            st.floppyType, static_cast<unsigned>(p.zip), st.readWrites);
}

void ApplyAssembledContent(coop::net::Session* s, uint8_t kind, uint32_t eid,
                           const std::string& bytes, uint8_t senderSlot) {
    (void)s;
    (void)eid;
    if (kind != 0) {
        UE_LOGW("laptop_sync: content blob kind=%u from slot %u -- this lane carries the LAPTOP "
                "SLOT only; a disc's own state rides PropSaveData by Key",
                static_cast<unsigned>(kind), static_cast<unsigned>(senderSlot));
        return;
    }
    // Laptop slot content: pair it with the parked scalars from the edge that preceded these
    // chunks in-lane, and land both in one write (the atomic occupied apply).
    L::SlotState st;
    auto pit = g_pendingSlots.find(senderSlot);
    if (pit != g_pendingSlots.end() && pit->second.valid) {
        st = pit->second.st;
        g_pendingSlots.erase(pit);
    } else if (!L::ReadSlot(st)) {
        return;
    }
    L::WriteSlot(st, UnpackSlotContent(bytes));
    PrimeBaselines();
    UE_LOGI("laptop_sync: slot scalars+content applied atomically (type=%d, %zu B, from slot %u)",
            st.floppyType, bytes.size(), static_cast<unsigned>(senderSlot));
}

// The portable-PC lid axis (op 6).

using coop::element::LivePropActor;  // the promoted canonical eid->live-Prop resolve

void SendLid(coop::net::Session* s, uint32_t eid, bool opened, int exceptSlot) {
    coop::net::LaptopStatePayload p{};
    p.op = 6;
    p.isOpened = opened ? 1 : 0;
    p.eid = eid;
    SendOut(s, p, exceptSlot);
}

// A 1 Hz element-snapshot walk with the pointer class gate (a verdict cache; no name
// rendering on the hot path, no object-array walk).
void LidSweep(coop::net::Session* s, uint64_t now) {
    if (now < g_nextLidSweep) return;
    g_nextLidSweep = now + kLidSweepMs;

    // Drain the unresolved-eid stash first (birth-lane skew heals here).
    for (auto it = g_lidPending.begin(); it != g_lidPending.end();) {
        void* actor = LivePropActor(it->first);
        if (actor && PPC::IsPortablePcClass(R::ClassOf(actor))) {
            bool cur = false;
            if (PPC::ReadOpened(actor, cur) && cur != it->second.opened)
                PPC::CallOpen(actor, it->second.opened);
            g_lidPrev[it->first] = it->second.opened;
            it = g_lidPending.erase(it);
            continue;
        }
        if (now > it->second.deadline) {
            UE_LOGW("laptop_sync: pending lid eid=%u EXPIRED unapplied (birth lane skew "
                    "exceeded the %llu ms TTL)", it->first,
                    static_cast<unsigned long long>(kPendingTtlMs));
            it = g_lidPending.erase(it);
            continue;
        }
        ++it;
    }

    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& pr : pairs) {
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!PPC::IsPortablePcClass(R::ClassOf(pr.actor))) continue;
        bool opened = false;
        if (!PPC::ReadOpened(pr.actor, opened)) continue;
        const uint32_t eid = static_cast<uint32_t>(pr.id);
        auto it = g_lidPrev.find(eid);
        if (it == g_lidPrev.end()) { g_lidPrev[eid] = opened; continue; }  // first sight: prime
        if (it->second == opened) continue;
        it->second = opened;
        if (s->connected()) {
            SendLid(s, eid, opened, -1);
            UE_LOGI("laptop_sync: local LID edge (eid=%u opened=%u) -- broadcast",
                    eid, static_cast<unsigned>(opened));
        }
    }
}

void ApplyPowerTarget(coop::net::Session* s) {
    (void)s;
    if (!g_wantValid) return;
    L::PowerState ps;
    if (!L::ReadPower(ps)) return;
    if (ps.isOpened == g_wantOpened) { g_wantValid = false; return; }  // converged
    if (ps.anim) return;               // boot/shutdown latent running -- retry next poll
    if (!ps.powered && g_wantOpened) return;  // wall power lags power_sync -- retry
    if (L::CallPowerToggle())
        UE_LOGI("laptop_sync: power replay dispatched (target isOpened=%u)",
                static_cast<unsigned>(g_wantOpened));
    // The opened flag settles after the native latent chain; the poll's want-target predicate
    // consumes that edge as wire-transient.
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const uint64_t now = NowMs();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollMs;

    if (!L::EnsureResolved()) return;
    void* inst = L::Instance();
    if (!inst) return;
    if (!g_announced) {
        g_announced = true;
        UE_LOGI("laptop_sync: installed (laptop resolved; power + floppy axes)");
    }
    if (!g_havePrev) { PrimeBaselines(); return; }

    // Expire half-assembled content streams (the chunk TTL sweep).
    g_blobAsm.Sweep(std::chrono::steady_clock::now(),
                    std::chrono::seconds(kChunkTtlMs / 1000));

    LidSweep(s, now);

    // The lost-content fallback: a parked occupied-slot edge whose chunk stream never assembled
    // applies scalar-only after the TTL (degraded, with a warning).
    for (auto it = g_pendingSlots.begin(); it != g_pendingSlots.end();) {
        if (it->second.valid && now > it->second.deadline) {
            UE_LOGW("laptop_sync: parked slot edge EXPIRED without content (type=%d, sender=%u) "
                    "-- scalar-only apply", it->second.st.floppyType,
                    static_cast<unsigned>(it->first));
            L::SlotContent empty;
            L::WriteSlot(it->second.st, empty);
            it = g_pendingSlots.erase(it);
            PrimeBaselines();
        } else {
            ++it;
        }
    }

    ApplyPowerTarget(s);

    L::PowerState ps;
    L::SlotState st;
    if (!L::ReadPower(ps) || !L::ReadSlot(st)) return;

    if (s->connected()) {
        // The power edge.
        if (ps.isOpened != g_prevOpened) {
            if (g_wantValid && ps.isOpened == g_wantOpened) {
                g_wantValid = false;  // wire-transient settle -- consume silently
            } else {
                coop::net::LaptopStatePayload p{};
                p.op = 0;
                p.isOpened = ps.isOpened ? 1 : 0;
                SendOut(s, p, -1);
                UE_LOGI("laptop_sync: local POWER edge (isOpened=%u) -- broadcast",
                        static_cast<unsigned>(p.isOpened));
            }
        }
        // The slot edge (the floppy type through -1).
        const bool wasEmpty = (g_prevType < 0);
        const bool isEmpty  = (st.floppyType < 0);
        if (wasEmpty && !isEmpty) {
            BroadcastInsert(s);
        } else if (!wasEmpty && isEmpty) {
            coop::net::LaptopStatePayload p{};
            p.op = 2;
            SendOut(s, p, -1);
            UE_LOGI("laptop_sync: local EJECT edge -- broadcast");
        }
    }

    g_prevOpened = ps.isOpened;
    g_prevType = st.floppyType;
}

void OnLaptopState(const coop::net::LaptopStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.op > 3 && p.op != 6) return;
    const bool isHost = (s->role() == coop::net::Role::Host);

    // The lid op does not need the laptop resolved: it addresses a portable-PC prop by eid. Apply
    // (gated on the current state differing from the wire) and prime; an unresolved eid is
    // stashed with a TTL (birth-lane skew).
    if (p.op == 6) {
        const bool opened = p.isOpened != 0;
        void* actor = LivePropActor(p.eid);
        if (actor && PPC::IsPortablePcClass(R::ClassOf(actor))) {
            bool cur = false;
            if (PPC::ReadOpened(actor, cur) && cur != opened) {
                if (PPC::CallOpen(actor, opened))
                    UE_LOGI("laptop_sync: wire LID applied (eid=%u opened=%u, from slot %u)",
                            p.eid, static_cast<unsigned>(opened),
                            static_cast<unsigned>(senderSlot));
            }
            g_lidPrev[p.eid] = opened;
        } else if (g_lidPending.size() < kLidPendingCap ||
                   g_lidPending.count(p.eid) != 0) {
            // The count term matters: refreshing an eid already pending is an update, not growth,
            // and must stay allowed at the cap, or a legitimate repeated lid edge would be refused
            // while its own entry sits in the table.
            g_lidPending[p.eid] = PendingLid{opened, NowMs() + kPendingTtlMs};
        } else {
            UE_LOGW("laptop_sync: lid stash REFUSED for eid=%u (from slot %u) -- pending "
                    "table at its %zu-entry bound; the inserting case is the "
                    "unresolved-eid case, so a wire eid stream cannot grow it",
                    p.eid, static_cast<unsigned>(senderSlot), kLidPendingCap);
        }
        if (isHost) SendOut(s, p, /*exceptSlot*/ senderSlot);
        return;
    }

    if (!L::EnsureResolved() || !L::Instance()) {
        UE_LOGW("laptop_sync: wire op=%u declined (laptop unresolved)", p.op);
        return;
    }

    switch (p.op) {
    case 0:   // power edge
    case 3: { // connect state
        g_wantValid = true;
        g_wantOpened = p.isOpened != 0;
        ApplyPowerTarget(s);
        if (p.op == 3) {
            L::SlotState st;
            st.floppyType = p.floppyType;
            st.zip = p.zip != 0;
            st.readWrites = p.readWrites;
            if (st.floppyType < 0) {
                // An empty slot: nothing follows, so apply the scalars now.
                L::WriteSlotScalars(st);
            } else {
                // Occupied: park until the content stream right behind lands; the scalars and
                // strings apply atomically there.
                PendingSlot& ps = g_pendingSlots[senderSlot];
                ps.valid = true;
                ps.sender = senderSlot;
                ps.st = st;
                ps.deadline = NowMs() + kChunkTtlMs;
            }
        }
        PrimeBaselines();
        break;
    }
    case 1: {  // the insert edge: park; the content stream follows in-lane, and the slot flips occupied only when the scalars and strings land together
        PendingSlot& ps = g_pendingSlots[senderSlot];
        ps.valid = true;
        ps.sender = senderSlot;
        ps.st.floppyType = p.floppyType;
        ps.st.zip = p.zip != 0;
        ps.st.readWrites = p.readWrites;
        ps.deadline = NowMs() + kChunkTtlMs;
        UE_LOGI("laptop_sync: wire INSERT parked pending content (type=%d, from slot %u)",
                p.floppyType, static_cast<unsigned>(senderSlot));
        break;
    }
    case 2: { // eject edge: clear scalars; the spawn arrives on the birth channels
        L::ClearSlot();
        PrimeBaselines();
        UE_LOGI("laptop_sync: wire EJECT applied (from slot %u)",
                static_cast<unsigned>(senderSlot));
        break;
    }
    default:
        return;
    }

    // The host re-fans the scalar ops to the other clients (the origin excluded).
    if (isHost && (p.op == 0 || p.op == 1 || p.op == 2))
        SendOut(s, p, /*exceptSlot*/ senderSlot);
}

void OnLaptopBlobChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool isHost = (s->role() == coop::net::Role::Host);
    // The host re-fans each chunk unchanged with the origin byte, so attribution stays stable at
    // every receiver's sender-and-sequence assembler keys; every client-originated content blob
    // is kind 0 or 1 by construction.
    if (isHost && senderSlot != 0 && senderSlot < coop::net::kMaxPeers) {
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
            if (slot == senderSlot || !s->IsSlotReady(slot)) continue;
            s->SendReliableToSlot(slot, coop::net::ReliableKind::LaptopBlob, &p, sizeof(p),
                                  senderSlot);
        }
    }
    std::vector<uint8_t> blob;
    if (!g_blobAsm.OnChunk(p, senderSlot, blob)) return;
    if (blob.size() < kBlobHead) return;
    const uint8_t kind = blob[0];
    if (kind > 1) return;
    uint32_t eid = 0;
    std::memcpy(&eid, blob.data() + 1, 4);
    if (!L::EnsureResolved() || !L::Instance()) {
        UE_LOGW("laptop_sync: content blob (kind=%u) declined (laptop unresolved)", kind);
        return;
    }
    const std::string bytes(reinterpret_cast<const char*>(blob.data()) + kBlobHead,
                            blob.size() - kBlobHead);
    ApplyAssembledContent(s, kind, eid, bytes, senderSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!L::EnsureResolved() || !L::Instance()) return;
    L::PowerState ps;
    L::SlotState st;
    L::SlotContent c;
    if (!L::ReadPower(ps) || !L::ReadSlot(st)) return;
    coop::net::LaptopStatePayload p{};
    p.op = 3;
    p.isOpened = ps.isOpened ? 1 : 0;
    p.zip = st.zip ? 1 : 0;
    p.floppyType = st.floppyType;
    p.readWrites = st.readWrites;
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::LaptopState, &p, sizeof(p));
    if (st.floppyType >= 0 && L::ReadSlotContent(c)) {
        // Point-to-point content toward the joiner only, in-lane after the state line (the state
        // and the blob share one lane, one FIFO).
        coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::LaptopBlob,
                                          g_blobSeq++, MakeContentBlob(0, 0, PackSlotContent(c)));
    }
    // Live discs are NOT seeded here any more: a disc's content is its save record, and
    // coop/props/prop_save_data seeds one per keyed prop by Key, after the prop snapshot.
    // The current lid state per portable PC (a runtime-only field; the joiner always arrives
    // lid-closed, so these rows are the only source).
    int lids = 0;
    {
        std::vector<coop::element::Registry::ActorIdPair> pairs;
        coop::element::Registry::Get().SnapshotActorsByType(
            coop::element::ElementType::Prop, pairs);
        for (const auto& pr : pairs) {
            if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
            if (!PPC::IsPortablePcClass(R::ClassOf(pr.actor))) continue;
            bool opened = false;
            if (!PPC::ReadOpened(pr.actor, opened)) continue;
            coop::net::LaptopStatePayload lp{};
            lp.op = 6;
            lp.isOpened = opened ? 1 : 0;
            lp.eid = static_cast<uint32_t>(pr.id);
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::LaptopState,
                                  &lp, sizeof(lp));
            ++lids;
        }
    }
    UE_LOGI("laptop_sync: connect state -> slot %d (isOpened=%u type=%d, %d lid row(s))",
            peerSlot, static_cast<unsigned>(p.isOpened), p.floppyType, lids);
}

void OnDisconnect() {
    g_havePrev = false;
    g_prevOpened = false;
    g_prevType = -1;
    g_nextPoll = 0;
    g_wantValid = false;
    g_blobAsm.Clear();
    g_pendingSlots.clear();
    g_lidPrev.clear();
    g_lidPending.clear();
    g_nextLidSweep = 0;
    g_announced = false;
    L::ResetCache();
    PPC::ResetCache();
}

}  // namespace coop::laptop_sync
