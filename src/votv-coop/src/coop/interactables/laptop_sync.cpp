// coop/interactables/laptop_sync.cpp -- see coop/interactables/laptop_sync.h.

#include "coop/interactables/laptop_sync.h"

#include "coop/comms/chat_feed.h"  // ToUtf8 (shared converter)
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/session.h"

#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::laptop_sync {
namespace {

namespace L = ue_wrap::laptop;
namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kPollMs        = 250;    // 4 Hz edge poll (the L7 cadence)
constexpr uint64_t kEjectWatchMs  = 10000;  // post-eject disc-content publish window
constexpr uint64_t kChunkTtlMs    = 10000;  // half-assembled content stream TTL
constexpr uint64_t kPendingTtlMs  = 30000;  // deferred disc-content apply TTL
constexpr size_t   kContentCapBytes = 4096; // total content cap (truncate + WARN; OPEN-9 residual)

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

// ---- poll baselines (primed on every wire apply -- the L7 apply+prime shape) ----
bool    g_havePrev = false;
bool    g_prevOpened = false;
int32_t g_prevType = -1;
uint64_t g_nextPoll = 0;

// Wire-target power convergence: a pending target consumes the matching local
// edge as WIRE-TRANSIENT (state predicate, not a flag-timer -- the
// edge-authority lesson); a non-matching edge is organic and broadcasts.
bool g_wantValid = false;
bool g_wantOpened = false;

// Post-eject disc-content publish watch (both roles; the client sends to the
// host, the host broadcasts).
uint64_t g_ejectWatchUntil = 0;
std::set<uint32_t> g_publishedContentEids;

// Content chunk assembly, keyed (senderSlot<<40 | kind<<32 | eid).
struct Assembly {
    std::string bytes;
    uint16_t total = 0;
    uint16_t next = 0;
    uint64_t deadline = 0;
};
std::map<uint64_t, Assembly> g_assembly;

// Deferred disc-content applies (mirror not materialized yet).
struct PendingDisc {
    L::DiscContent content;
    uint64_t deadline = 0;
};
std::map<uint32_t, PendingDisc> g_pendingDisc;

// The OCCUPIED-slot scalar edge is NOT applied on arrival: it parks here until
// its kind=0 content stream assembles, and slot scalars + strings land in ONE
// WriteSlot (correctness audit v116 IMPORTANT-1: applying scalars with empty
// strings first opened a window where a local eject on the receiver spawned a
// content-less disc that became the canonical cross-peer prop). In-lane
// ordering guarantees the chunks follow their edge; the TTL is the lost-stream
// fallback (degraded scalar-only apply + WARN, no worse than the old shape).
struct PendingSlot {
    bool valid = false;
    uint8_t sender = 0xFF;
    L::SlotState st;
    uint64_t deadline = 0;
};
PendingSlot g_pendingSlot;

bool g_announced = false;

// ---- serialization (fields joined by 0x1F, UTF-8) ----
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

std::string PackDiscContent(const L::DiscContent& c) {
    std::string out = std::to_string(c.readWrites);
    for (const auto& d : c.data) { out += kSep; out += coop::chat_feed::ToUtf8(d); }
    return out;
}

L::DiscContent UnpackDiscContent(const std::string& bytes) {
    L::DiscContent c;
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= bytes.size(); ++i) {
        if (i == bytes.size() || bytes[i] == kSep) {
            parts.push_back(bytes.substr(start, i - start));
            start = i + 1;
        }
    }
    if (!parts.empty()) c.readWrites = atoi(parts[0].c_str());
    for (size_t i = 1; i < parts.size(); ++i) c.data.push_back(FromUtf8(parts[i]));
    return c;
}

// ---- send helpers ----
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

// Chunk a content string into op=4 payloads toward one destination
// (exceptSlot=-1 broadcast semantics of SendOut).
void SendContent(coop::net::Session* s, uint8_t kind, uint32_t eid,
                 const std::string& bytes, int exceptSlot) {
    std::string data = bytes;
    if (data.size() > kContentCapBytes) {
        UE_LOGW("laptop_sync: content (kind=%u eid=%u) %zu B over the %zu cap -- TRUNCATED "
                "(OPEN-9 residual)", kind, eid, data.size(), kContentCapBytes);
        data.resize(kContentCapBytes);
    }
    const size_t chunkCap = sizeof(coop::net::LaptopStatePayload{}.content);
    const uint16_t total = static_cast<uint16_t>((data.size() + chunkCap - 1) / chunkCap);
    for (uint16_t seq = 0; seq < total || (seq == 0 && total == 0); ++seq) {
        coop::net::LaptopStatePayload p{};
        p.op = 4;
        p.contentKind = kind;
        p.eid = eid;
        p.chunkSeq = seq;
        p.chunkTotal = total == 0 ? 1 : total;
        const size_t off = static_cast<size_t>(seq) * chunkCap;
        const size_t len = off < data.size()
            ? (data.size() - off < chunkCap ? data.size() - off : chunkCap) : 0;
        p.contentLen = static_cast<uint16_t>(len);
        if (len) std::memcpy(p.content, data.data() + off, len);
        SendOut(s, p, exceptSlot);
        if (total == 0) break;
    }
}

void PrimeBaselines() {
    L::PowerState ps;
    L::SlotState st;
    if (L::ReadPower(ps) && L::ReadSlot(st)) {
        g_prevOpened = ps.isOpened;
        g_prevType = st.floppyType;
        g_havePrev = true;
    }
}

// The local slot-content publish for an INSERT edge (organic or replayed-onto-
// us never happens -- receivers write raw, primed). Reads the laptop's live
// scalars + strings and ships edge + chunks.
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
    SendContent(s, /*kind*/0, /*eid*/0, PackSlotContent(c), -1);
    UE_LOGI("laptop_sync: local INSERT edge (type=%d zip=%u rw=%d) -- broadcast + content",
            st.floppyType, static_cast<unsigned>(p.zip), st.readWrites);
}

// The post-eject content publish: find a content-bearing disc row not yet
// published; client sends it to the host (authority), host broadcasts.
// Bounded: runs only inside the 10 s post-eject window, cheap class gate
// first (the full-array-walk lesson does not apply -- this walks the element
// SNAPSHOT, not GUObjectArray).
void DriveEjectContentWatch(coop::net::Session* s, uint64_t now) {
    if (!g_ejectWatchUntil) return;
    if (now > g_ejectWatchUntil) { g_ejectWatchUntil = 0; return; }
    static std::vector<coop::element::Prop*> rows;
    coop::element::MirrorManager<coop::element::Prop>::Instance().Snapshot(rows);
    for (coop::element::Prop* row : rows) {
        if (!row) continue;
        void* actor = row->GetActor();
        if (!actor) continue;
        void* cls = R::ClassOf(actor);
        if (!L::IsDiscClass(cls)) continue;
        const uint32_t eid = static_cast<uint32_t>(row->GetId());
        if (!eid || g_publishedContentEids.count(eid)) continue;
        L::DiscContent dc;
        if (!L::ReadDiscContent(actor, dc) || dc.data.empty()) continue;
        g_publishedContentEids.insert(eid);
        g_ejectWatchUntil = 0;
        SendContent(s, /*kind*/1, eid, PackDiscContent(dc), -1);
        UE_LOGI("laptop_sync: post-eject disc content published (eid=%u, %zu string(s), rw=%d)",
                eid, dc.data.size(), dc.readWrites);
        return;
    }
}

void DrivePendingDiscApplies(uint64_t now) {
    for (auto it = g_pendingDisc.begin(); it != g_pendingDisc.end();) {
        coop::element::Prop* row =
            coop::element::MirrorManager<coop::element::Prop>::Instance().Get(it->first);
        void* actor = row ? row->GetActor() : nullptr;
        if (actor) {
            if (L::WriteDiscContent(actor, it->second.content))
                UE_LOGI("laptop_sync: deferred disc content applied (eid=%u)", it->first);
            it = g_pendingDisc.erase(it);
            continue;
        }
        if (now > it->second.deadline) {
            UE_LOGW("laptop_sync: deferred disc content EXPIRED unapplied (eid=%u -- mirror "
                    "never materialized)", it->first);
            it = g_pendingDisc.erase(it);
            continue;
        }
        ++it;
    }
}

void ApplyAssembledContent(coop::net::Session* s, uint8_t kind, uint32_t eid,
                           const std::string& bytes, uint8_t senderSlot) {
    if (kind == 0) {
        // Laptop slot content -- pair it with the PARKED scalars from the op=1/3
        // edge that preceded these chunks in-lane, and land both in ONE
        // WriteSlot (atomic occupied-apply; audit IMPORTANT-1).
        L::SlotState st;
        if (g_pendingSlot.valid && g_pendingSlot.sender == senderSlot) {
            st = g_pendingSlot.st;
            g_pendingSlot = PendingSlot{};
        } else if (!L::ReadSlot(st)) {
            return;
        }
        L::WriteSlot(st, UnpackSlotContent(bytes));
        PrimeBaselines();
        UE_LOGI("laptop_sync: slot scalars+content applied atomically (type=%d, %zu B, "
                "from slot %u)", st.floppyType, bytes.size(),
                static_cast<unsigned>(senderSlot));
        return;
    }
    // kind == 1: disc content by eid. HOST: write the authoritative actor
    // (or defer) THEN re-fan (the content authority step); CLIENT: write the
    // mirror or defer.
    const L::DiscContent dc = UnpackDiscContent(bytes);
    coop::element::Prop* row =
        coop::element::MirrorManager<coop::element::Prop>::Instance().Get(eid);
    void* actor = row ? row->GetActor() : nullptr;
    if (actor && L::WriteDiscContent(actor, dc)) {
        UE_LOGI("laptop_sync: disc content applied (eid=%u, from slot %u)",
                eid, static_cast<unsigned>(senderSlot));
    } else {
        PendingDisc pd;
        pd.content = dc;
        pd.deadline = NowMs() + kPendingTtlMs;
        g_pendingDisc[eid] = pd;
        UE_LOGI("laptop_sync: disc content deferred (eid=%u not materialized yet)", eid);
    }
    g_publishedContentEids.insert(eid);  // idempotence across the watch + wire
    if (s->role() == coop::net::Role::Host)
        SendContent(s, 1, eid, bytes, /*exceptSlot*/ senderSlot);
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
    // isOpened settles after the native latent chain; the poll's want-target
    // predicate consumes that edge as wire-transient.
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

    // Expire half-assembled content streams.
    for (auto it = g_assembly.begin(); it != g_assembly.end();) {
        if (now > it->second.deadline) it = g_assembly.erase(it);
        else ++it;
    }

    // Lost-content fallback: a parked occupied-slot edge whose chunk stream
    // never assembled applies scalar-only after the TTL (degraded, WARN).
    if (g_pendingSlot.valid && now > g_pendingSlot.deadline) {
        UE_LOGW("laptop_sync: parked slot edge EXPIRED without content (type=%d, sender=%u) "
                "-- scalar-only apply", g_pendingSlot.st.floppyType,
                static_cast<unsigned>(g_pendingSlot.sender));
        L::SlotContent empty;
        L::WriteSlot(g_pendingSlot.st, empty);
        g_pendingSlot = PendingSlot{};
        PrimeBaselines();
    }

    DrivePendingDiscApplies(now);
    ApplyPowerTarget(s);

    L::PowerState ps;
    L::SlotState st;
    if (!L::ReadPower(ps) || !L::ReadSlot(st)) return;

    if (s->connected()) {
        // POWER edge.
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
        // SLOT edge (floppyType through -1).
        const bool wasEmpty = (g_prevType < 0);
        const bool isEmpty  = (st.floppyType < 0);
        if (wasEmpty && !isEmpty) {
            BroadcastInsert(s);
        } else if (!wasEmpty && isEmpty) {
            coop::net::LaptopStatePayload p{};
            p.op = 2;
            SendOut(s, p, -1);
            g_ejectWatchUntil = now + kEjectWatchMs;
            UE_LOGI("laptop_sync: local EJECT edge -- broadcast (content watch armed)");
        }
        DriveEjectContentWatch(s, now);
    }

    g_prevOpened = ps.isOpened;
    g_prevType = st.floppyType;
}

void OnLaptopState(const coop::net::LaptopStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.op > 4 || p.contentLen > sizeof(p.content)) return;
    if (!L::EnsureResolved() || !L::Instance()) {
        UE_LOGW("laptop_sync: wire op=%u declined (laptop unresolved)", p.op);
        return;
    }
    const bool isHost = (s->role() == coop::net::Role::Host);

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
                // Empty slot: nothing follows -- apply the scalars now.
                L::WriteSlotScalars(st);
            } else {
                // Occupied: PARK until the kind=0 stream right behind lands;
                // scalars + strings apply atomically there (audit IMPORTANT-1).
                g_pendingSlot.valid = true;
                g_pendingSlot.sender = senderSlot;
                g_pendingSlot.st = st;
                g_pendingSlot.deadline = NowMs() + kChunkTtlMs;
            }
        }
        PrimeBaselines();
        break;
    }
    case 1: { // insert edge: PARK -- the content stream follows in-lane; the
              // slot flips occupied only when scalars + strings land together
              // (audit IMPORTANT-1: no content-empty occupied window).
        g_pendingSlot.valid = true;
        g_pendingSlot.sender = senderSlot;
        g_pendingSlot.st.floppyType = p.floppyType;
        g_pendingSlot.st.zip = p.zip != 0;
        g_pendingSlot.st.readWrites = p.readWrites;
        g_pendingSlot.deadline = NowMs() + kChunkTtlMs;
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
    case 4: { // content chunk
        if (p.chunkTotal == 0 || p.chunkSeq >= p.chunkTotal || p.contentKind > 1) return;
        // HOST refan: kind=0 (slot content) forwards verbatim per chunk (order
        // preserved in-lane); kind=1 (disc content) refans AFTER the host's
        // authoritative apply, inside ApplyAssembledContent.
        if (isHost && p.contentKind == 0)
            SendOut(s, p, /*exceptSlot*/ senderSlot);
        const uint64_t key = (static_cast<uint64_t>(senderSlot) << 40) |
                             (static_cast<uint64_t>(p.contentKind) << 32) | p.eid;
        Assembly& a = g_assembly[key];
        if (p.chunkSeq == 0) { a.bytes.clear(); a.total = p.chunkTotal; a.next = 0; }
        if (p.chunkSeq != a.next || p.chunkTotal != a.total) {
            // In-lane ordering makes this unreachable except after a dropped
            // stream restart -- resync by dropping the assembly.
            g_assembly.erase(key);
            return;
        }
        a.bytes.append(p.content, p.contentLen);
        a.next++;
        a.deadline = NowMs() + kChunkTtlMs;
        if (a.next == a.total) {
            const std::string bytes = std::move(a.bytes);
            g_assembly.erase(key);
            ApplyAssembledContent(s, p.contentKind, p.eid, bytes, senderSlot);
        }
        return;
    }
    default:
        return;
    }

    // HOST re-fans the scalar ops to the other clients (origin excluded).
    if (isHost && (p.op == 0 || p.op == 1 || p.op == 2))
        SendOut(s, p, /*exceptSlot*/ senderSlot);
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
        // Point-to-point content chunks toward the joiner only.
        const std::string bytes = PackSlotContent(c);
        std::string data = bytes.size() > kContentCapBytes
            ? bytes.substr(0, kContentCapBytes) : bytes;
        const size_t chunkCap = sizeof(coop::net::LaptopStatePayload{}.content);
        const uint16_t total = static_cast<uint16_t>((data.size() + chunkCap - 1) / chunkCap);
        for (uint16_t seq = 0; seq < total; ++seq) {
            coop::net::LaptopStatePayload cp{};
            cp.op = 4; cp.contentKind = 0; cp.eid = 0;
            cp.chunkSeq = seq; cp.chunkTotal = total;
            const size_t off = static_cast<size_t>(seq) * chunkCap;
            const size_t len = data.size() - off < chunkCap ? data.size() - off : chunkCap;
            cp.contentLen = static_cast<uint16_t>(len);
            std::memcpy(cp.content, data.data() + off, len);
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::LaptopState, &cp, sizeof(cp));
        }
    }
    // Live content-bearing discs (mid-session ejects the save-transfer cannot
    // carry) -- GROUND TRUTH read off the element snapshot, no bookkeeping.
    static std::vector<coop::element::Prop*> rows;
    coop::element::MirrorManager<coop::element::Prop>::Instance().Snapshot(rows);
    int shipped = 0;
    for (coop::element::Prop* row : rows) {
        if (!row) continue;
        void* actor = row->GetActor();
        if (!actor) continue;
        if (!L::IsDiscClass(R::ClassOf(actor))) continue;
        L::DiscContent dc;
        if (!L::ReadDiscContent(actor, dc) || dc.data.empty()) continue;
        const uint32_t eid = static_cast<uint32_t>(row->GetId());
        if (!eid) continue;
        const std::string bytes = PackDiscContent(dc);
        const size_t chunkCap = sizeof(coop::net::LaptopStatePayload{}.content);
        std::string data = bytes.size() > kContentCapBytes
            ? bytes.substr(0, kContentCapBytes) : bytes;
        const uint16_t total = static_cast<uint16_t>((data.size() + chunkCap - 1) / chunkCap);
        for (uint16_t seq = 0; seq < total; ++seq) {
            coop::net::LaptopStatePayload cp{};
            cp.op = 4; cp.contentKind = 1; cp.eid = eid;
            cp.chunkSeq = seq; cp.chunkTotal = total;
            const size_t off = static_cast<size_t>(seq) * chunkCap;
            const size_t len = data.size() - off < chunkCap ? data.size() - off : chunkCap;
            cp.contentLen = static_cast<uint16_t>(len);
            std::memcpy(cp.content, data.data() + off, len);
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::LaptopState, &cp, sizeof(cp));
        }
        ++shipped;
    }
    UE_LOGI("laptop_sync: connect state -> slot %d (isOpened=%u type=%d, %d disc content row(s))",
            peerSlot, static_cast<unsigned>(p.isOpened), p.floppyType, shipped);
}

void OnDisconnect() {
    g_havePrev = false;
    g_prevOpened = false;
    g_prevType = -1;
    g_nextPoll = 0;
    g_wantValid = false;
    g_ejectWatchUntil = 0;
    g_publishedContentEids.clear();
    g_assembly.clear();
    g_pendingDisc.clear();
    g_pendingSlot = PendingSlot{};
    g_announced = false;
    L::ResetCache();
}

}  // namespace coop::laptop_sync
