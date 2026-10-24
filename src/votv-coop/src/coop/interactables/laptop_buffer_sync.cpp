// coop/interactables/laptop_buffer_sync.cpp -- see the header.

#include "coop/interactables/laptop_buffer_sync.h"

#include "coop/comms/chat_feed.h"  // ToUtf8
#include "coop/interactables/floppy_slot_sync.h"  // the laptop's generation and whether its occupancy is settled
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady (client send gate)

#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace coop::laptop_buffer_sync {
namespace {

namespace L = ue_wrap::laptop;
namespace GT = ue_wrap::game_thread;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kPollMs = 250;  // 4 Hz (the laptop_sync cadence)

uint64_t NowMs() { return static_cast<uint64_t>(::GetTickCount64()); }

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The wire's string encoder: UTF-8 of every code unit, control characters included. coop::chat_feed::ToUtf8 is a chat
// sanitiser and drops CR and LF, which a disc's rows are made of ("$t", "Server data: ...", "/n kilo", "/d " are
// separate lines). A row sent through it arrived with its line breaks gone, so the laptop could not parse it.
std::string ToUtf8Exact(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

uint64_t HashStr(const std::wstring& w) {
    const std::string u = ToUtf8Exact(w);
    return coop::blob_chunks::Fnv64(reinterpret_cast<const uint8_t*>(u.data()), u.size());
}

// SEQUENCE hash of the quad (order-sensitive by design: the change detector
// must see a net-zero-multiset order change -- R3/R6).
uint64_t QuadSeqHash(const L::BufferQuad& q) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    };
    mix(0xF00Dull);
    for (const auto& s : q.data) mix(HashStr(s));
    mix(0xBEEFull);
    for (const auto& s : q.buffer) mix(HashStr(s));
    mix(0xCAFEull);
    for (int32_t u : q.bufferUids) mix(static_cast<uint64_t>(static_cast<uint32_t>(u)));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(q.readWrites)));
    return h;
}

// ---- state ----
bool g_havePrev = false;
L::BufferQuad g_prev;          // the shadow (kept at SENT state on clients)
uint64_t g_prevHash = 0;
int32_t g_prevInts[4] = {-1, -1, -1, -1};  // {fdN, fbN, uidN, rw} pre-filter cache
uint64_t g_nextPoll = 0;
bool g_announced = false;

coop::blob_chunks::Assembler g_asm;
uint32_t g_nextSeq = 1;
bool g_canonRetry = false;  // a refused canonical send retries each poll
Counts g_counts;
bool g_devEjectAtNextBatch = false;  // [dev] HOST: the laptop drill's eject racing the next batch

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// ---- wire serialization ----
// batch (op 0): [u8 0][u32 generation][u16 n]{ [u8 kind 0=removeAt|1=appendTail][u8 arrayId 0=fd|1=fb]
//               [u16 idx][i32 uid][u64 hash][u16 strLen][utf8...] }[i32 rwDelta]
// canonical (op 1): [u8 1][u32 generation][i32 rw][u16 fdN]{u16 len+utf8}[u16 fbN]{...}[u16 uidN]{i32}
// The generation is the slot lane's for the laptop (floppy_slot_sync::LaptopGeneration): which disc the edit is on.

void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void PutStr(std::vector<uint8_t>& b, const std::wstring& w) {
    std::string u = ToUtf8Exact(w);
    if (u.size() > 0xFFFF) u.resize(0xFFFF);
    PutU16(b, static_cast<uint16_t>(u.size()));
    b.insert(b.end(), u.begin(), u.end());
}

struct Reader {
    const uint8_t* p; size_t n, off = 0;
    bool ok = true;
    uint8_t U8() { if (off + 1 > n) { ok = false; return 0; } return p[off++]; }
    uint16_t U16() { if (off + 2 > n) { ok = false; return 0; } uint16_t v = static_cast<uint16_t>(p[off] | (p[off+1] << 8)); off += 2; return v; }
    uint32_t U32() { if (off + 4 > n) { ok = false; return 0; } uint32_t v = 0; std::memcpy(&v, p + off, 4); off += 4; return v; }
    uint64_t U64() { if (off + 8 > n) { ok = false; return 0; } uint64_t v = 0; std::memcpy(&v, p + off, 8); off += 8; return v; }
    std::wstring Str() {
        const uint16_t len = U16();
        if (!ok || off + len > n) { ok = false; return std::wstring(); }
        std::string u(reinterpret_cast<const char*>(p + off), len);
        off += len;
        return FromUtf8(u);
    }
};

struct EditOp {
    uint8_t kind;     // 0=removeAt, 1=appendTail
    uint8_t arrayId;  // 0=floppyData, 1=floppyBuffer
    uint16_t idx;     // removeAt live index
    int32_t uid;      // appendTail(fb): the parallel UID
    uint64_t hash;    // removeAt content check
    std::wstring str; // appendTail payload
};

// Greedy prev->cur walk under the native grammar (removals + tail appends).
// PROOF (R-impl): j advances ONLY on match, so survivors are exactly
// cur[0..j) and the appended tail is cur[j..) -- the script applied to prev
// ALWAYS yields cur.
void DeriveArray(uint8_t arrayId, const std::vector<std::wstring>& prev,
                 const std::vector<std::wstring>& cur,
                 const std::vector<int32_t>* curUids, std::vector<EditOp>& ops) {
    size_t j = 0, removed = 0;
    for (size_t i = 0; i < prev.size(); ++i) {
        if (j < cur.size() && prev[i] == cur[j]) { ++j; continue; }
        EditOp op{};
        op.kind = 0; op.arrayId = arrayId;
        op.idx = static_cast<uint16_t>(i - removed);
        op.hash = HashStr(prev[i]);
        ops.push_back(std::move(op));
        ++removed;
    }
    for (; j < cur.size(); ++j) {
        EditOp op{};
        op.kind = 1; op.arrayId = arrayId;
        op.uid = (curUids && j < curUids->size()) ? (*curUids)[j] : 0;
        op.str = cur[j];
        ops.push_back(std::move(op));
    }
}

std::vector<uint8_t> PackBatch(const std::vector<EditOp>& ops, int32_t rwDelta, uint32_t gen) {
    std::vector<uint8_t> b;
    b.push_back(0);
    PutU32(b, gen);
    PutU16(b, static_cast<uint16_t>(ops.size()));
    for (const auto& op : ops) {
        b.push_back(op.kind);
        b.push_back(op.arrayId);
        PutU16(b, op.idx);
        PutU32(b, static_cast<uint32_t>(op.uid));
        PutU64(b, op.hash);
        if (op.kind == 1) PutStr(b, op.str);
    }
    PutU32(b, static_cast<uint32_t>(rwDelta));
    return b;
}

std::vector<uint8_t> PackCanonical(const L::BufferQuad& q, uint32_t gen) {
    std::vector<uint8_t> b;
    b.push_back(1);
    PutU32(b, gen);
    PutU32(b, static_cast<uint32_t>(q.readWrites));
    PutU16(b, static_cast<uint16_t>(q.data.size()));
    for (const auto& s : q.data) PutStr(b, s);
    PutU16(b, static_cast<uint16_t>(q.buffer.size()));
    for (const auto& s : q.buffer) PutStr(b, s);
    PutU16(b, static_cast<uint16_t>(q.bufferUids.size()));
    for (int32_t u : q.bufferUids) PutU32(b, static_cast<uint32_t>(u));
    return b;
}

// The transport hard-caps a blob at MaxBlobBytes(), and a failed send on the
// canonical path is a SILENT divergence. Bound the canonical BELOW the cap
// deterministically (drop TAIL buffer rows first, then tail fd rows) with a WARN --
// the same accepted-residual class as the laptop content cap.
std::vector<uint8_t> PackCanonicalBounded(L::BufferQuad q, uint32_t gen) {
    std::vector<uint8_t> b = PackCanonical(q, gen);
    int dropped = 0;
    while (b.size() > coop::blob_chunks::MaxBlobBytes() &&
           (!q.buffer.empty() || !q.data.empty())) {
        if (!q.buffer.empty()) {
            q.buffer.pop_back();
            if (!q.bufferUids.empty()) q.bufferUids.pop_back();
        } else {
            q.data.pop_back();
        }
        ++dropped;
        b = PackCanonical(q, gen);
    }
    if (dropped)
        UE_LOGW("laptop_buffer: canonical quad over the %zu B transport cap -- %d tail "
                "row(s) dropped (OPEN-9-class residual)",
                coop::blob_chunks::MaxBlobBytes(), dropped);
    return b;
}

bool ParseCanonical(Reader& r, L::BufferQuad& q) {
    q.readWrites = static_cast<int32_t>(r.U32());
    uint16_t n = r.U16();
    for (uint16_t i = 0; i < n && r.ok; ++i) q.data.push_back(r.Str());
    n = r.U16();
    for (uint16_t i = 0; i < n && r.ok; ++i) q.buffer.push_back(r.Str());
    n = r.U16();
    for (uint16_t i = 0; i < n && r.ok; ++i) q.bufferUids.push_back(static_cast<int32_t>(r.U32()));
    return r.ok;
}

void PrimeFrom(const L::BufferQuad& q) {
    g_prev = q;
    g_prevHash = QuadSeqHash(q);
    g_prevInts[0] = static_cast<int32_t>(q.data.size());
    g_prevInts[1] = static_cast<int32_t>(q.buffer.size());
    g_prevInts[2] = static_cast<int32_t>(q.bufferUids.size());
    g_prevInts[3] = q.readWrites;
    g_havePrev = true;
}

// The R3 int pre-filter: true when NOTHING moved (every native verb changes
// at least one of the four ints -- rw monotone proof; no combo nets zero).
bool IntsUnchanged() {
    int32_t fd = 0, fb = 0, uid = 0, rw = 0;
    if (!L::ReadQuadInts(fd, fb, uid, rw)) return true;  // unreadable = no edge
    return fd == g_prevInts[0] && fb == g_prevInts[1] &&
           uid == g_prevInts[2] && rw == g_prevInts[3];
}

bool AnyClientReady(coop::net::Session* s) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
        if (s->IsSlotWorldReady(slot)) return true;
    return false;
}

void HostBroadcastCanonical(coop::net::Session* s) {
    // A change of the laptop's occupancy the slot lane has not published goes out first: its canonical names the disc.
    if (!coop::floppy_slot_sync::LaptopOccupancySettled()) {
        g_canonRetry = true;
        return;
    }
    L::BufferQuad q;
    if (!L::ReadQuad(q)) return;
    // No READY client -> nothing to deliver: prime silently (a loading joiner
    // gets the connect canonical at its ready edge -- the B2 doctrine; smoke 1
    // caught the 4 Hz refused-WARN spam of sending into the load window).
    if (!AnyClientReady(s)) {
        PrimeFrom(q);
        g_canonRetry = false;
        return;
    }
    // The canonical IS the ack, so a refused send must NOT prime (the detector
    // re-fires) and arms the per-poll retry. A bounded pack cannot hit the oversize
    // false; a false here is backpressure.
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::LaptopQuad, g_nextSeq++,
                                    PackCanonicalBounded(q, coop::floppy_slot_sync::LaptopGeneration()))) {
        PrimeFrom(q);
        g_canonRetry = false;
    } else {
        if (!g_canonRetry)
            UE_LOGW("laptop_buffer: canonical send refused (backpressure) -- retry armed");
        g_canonRetry = true;
    }
}

// Apply a batch on the HOST quad. Index-anchored removals with the hash check
// at idx; mismatch -> content-search fallback; still-miss -> SKIP (errs toward
// KEEPING data -- the unconditional canonical returns kept rows to the author).
void HostApplyBatch(Reader& r, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (g_devEjectAtNextBatch) {
        g_devEjectAtNextBatch = false;
        const bool ejected = L::CallEjectDisc();
        UE_LOGI("laptop_buffer: [dev] the laptop's disc ejected ahead of slot %u's batch (%s)",
                static_cast<unsigned>(senderSlot), ejected ? "ok" : "the eject failed");
    }
    // A batch made on another disc than the one the laptop holds here is not an edit of it: the canonical answers. The
    // host's own change of the disc counts from the moment it is made, before the slot lane publishes it.
    const uint32_t gen = r.U32();
    if (!r.ok || gen != coop::floppy_slot_sync::LaptopGeneration() ||
        !coop::floppy_slot_sync::LaptopOccupancySettled()) {
        ++g_counts.batchesRefused;
        UE_LOGI("laptop_buffer: batch from slot %u on generation %u refused -- the laptop is on %u here%s; canonical "
                "follows", static_cast<unsigned>(senderSlot), gen, coop::floppy_slot_sync::LaptopGeneration(),
                coop::floppy_slot_sync::LaptopOccupancySettled() ? "" : " and its disc has changed since");
        HostBroadcastCanonical(s);
        return;
    }
    L::BufferQuad q;
    if (!L::ReadQuad(q)) return;
    const uint16_t n = r.U16();
    int applied = 0, skipped = 0;
    for (uint16_t i = 0; i < n && r.ok; ++i) {
        EditOp op{};
        op.kind = r.U8();
        op.arrayId = r.U8();
        op.idx = r.U16();
        op.uid = static_cast<int32_t>(r.U32());
        op.hash = r.U64();
        if (op.kind == 1) op.str = r.Str();
        if (!r.ok) break;
        auto& arr = (op.arrayId == 0) ? q.data : q.buffer;
        if (op.kind == 0) {
            int32_t at = -1;
            if (op.idx < arr.size() && HashStr(arr[op.idx]) == op.hash) {
                at = op.idx;
            } else {
                for (size_t k = 0; k < arr.size(); ++k)
                    if (HashStr(arr[k]) == op.hash) { at = static_cast<int32_t>(k); break; }
            }
            if (at < 0) {
                ++skipped;
                UE_LOGW("laptop_buffer: batch removeAt miss (arr=%u idx=%u hash=%016llx, "
                        "from slot %u) -- SKIPPED (canonical heals)",
                        op.arrayId, op.idx,
                        static_cast<unsigned long long>(op.hash),
                        static_cast<unsigned>(senderSlot));
                continue;
            }
            arr.erase(arr.begin() + at);
            if (op.arrayId == 1 && static_cast<size_t>(at) < q.bufferUids.size())
                q.bufferUids.erase(q.bufferUids.begin() + at);
            ++applied;
        } else {
            arr.push_back(op.str);
            if (op.arrayId == 1) q.bufferUids.push_back(op.uid);
            ++applied;
        }
    }
    const int32_t rwDelta = static_cast<int32_t>(r.U32());
    if (r.ok && rwDelta) {
        q.readWrites += rwDelta;
        if (q.readWrites < 0) q.readWrites = 0;
    }
    if (!r.ok) {
        UE_LOGW("laptop_buffer: malformed batch from slot %u -- dropped (canonical follows)",
                static_cast<unsigned>(senderSlot));
    } else if (const size_t packed = PackCanonical(q, gen).size();
               packed > coop::blob_chunks::MaxBlobBytes()) {
        // appendTail has NO cap of its own, so a wire batch could grow the host's engine
        // arrays without bound. The bound is NOT invented here -- it already exists one
        // function away: PackCanonicalBounded drops TAIL rows to fit MaxBlobBytes(), so any
        // state past that point is one the canonical cannot carry. Applying it would grow
        // the engine array into a region the ack silently truncates, which is unbounded
        // growth AND permanent divergence at once. Refuse the WHOLE batch (a
        // partially-applied edit script is not an edit anyone authored) and let the
        // unconditional canonical below heal the sender.
        UE_LOGW("laptop_buffer: batch from slot %u REFUSED -- the applied quad would pack "
                "to %zu B, past the %zu B transport cap; nothing written, canonical follows",
                static_cast<unsigned>(senderSlot), packed, coop::blob_chunks::MaxBlobBytes());
    } else {
        L::WriteQuadAndRebuild(q);
        UE_LOGI("laptop_buffer: batch from slot %u applied (%d op(s), %d skipped, rwDelta=%d)",
                static_cast<unsigned>(senderSlot), applied, skipped, rwDelta);
    }
    // The UNCONDITIONAL post-batch canonical (the ack) -- also primes.
    HostBroadcastCanonical(s);
}

// CLIENT canonical adopt: drain-before-adopt (ship own pending diff first),
// then skip-rebuild-on-equal / full rebuild.
void DeriveAndSendLocal(coop::net::Session* s);

void ClientAdoptCanonical(Reader& r) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const uint32_t gen = r.U32();
    L::BufferQuad wire;
    if (!ParseCanonical(r, wire)) {
        UE_LOGW("laptop_buffer: malformed canonical -- dropped");
        return;
    }
    // A canonical of another disc than the one the laptop holds here is not applied to it: one of another generation,
    // or any while this client's own change of the disc waits for the host, whose answer and canonical follow here.
    if (gen != coop::floppy_slot_sync::LaptopGeneration() || !coop::floppy_slot_sync::LaptopOccupancySettled()) {
        ++g_counts.canonicalsDropped;
        UE_LOGI("laptop_buffer: canonical on generation %u dropped -- the laptop is on %u here%s", gen,
                coop::floppy_slot_sync::LaptopGeneration(),
                coop::floppy_slot_sync::LaptopOccupancySettled() ? "" : " and its own change of the disc is unanswered");
        return;
    }
    DeriveAndSendLocal(s);  // drain before adopt, the shape drive_rack_sync uses
    const uint64_t wireHash = QuadSeqHash(wire);
    L::BufferQuad local;
    ++g_counts.canonicalsTaken;
    if (L::ReadQuad(local) && QuadSeqHash(local) == wireHash) {
        PrimeFrom(local);  // skip-rebuild-on-equal (the echo case -- no flash)
        return;
    }
    if (!L::WriteQuadAndRebuild(wire))
        UE_LOGW("laptop_buffer: canonical adopt -- widget rebuild unreachable "
                "(fields written; rebuild retries at the next canonical)");
    PrimeFrom(wire);
    UE_LOGI("laptop_buffer: canonical adopted (fd=%zu fb=%zu rw=%d)",
            wire.data.size(), wire.buffer.size(), wire.readWrites);
}

// Derive the local edit script vs the shadow and send it (client author path).
// Shadow advances to SENT only when the whole batch blob was accepted (I-3).
void DeriveAndSendLocal(coop::net::Session* s) {
    if (IsHost() || !g_havePrev) return;
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;  // pre-ready = save echo
    if (IntsUnchanged()) return;  // the R3 pre-filter (no string reads on the idle path)
    // Held while this client's own change of the laptop's occupancy waits for the host: the edit is on that disc.
    if (!coop::floppy_slot_sync::LaptopOccupancySettled()) return;
    L::BufferQuad cur;
    if (!L::ReadQuad(cur)) return;
    const uint64_t h = QuadSeqHash(cur);
    if (h == g_prevHash) return;
    std::vector<EditOp> ops;
    DeriveArray(0, g_prev.data, cur.data, nullptr, ops);
    DeriveArray(1, g_prev.buffer, cur.buffer, &cur.bufferUids, ops);
    const int32_t rwDelta = cur.readWrites - g_prev.readWrites;
    if (ops.empty() && rwDelta == 0) { PrimeFrom(cur); return; }  // uid-only churn: prime
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::LaptopQuad, g_nextSeq++,
                                    PackBatch(ops, rwDelta, coop::floppy_slot_sync::LaptopGeneration()))) {
        PrimeFrom(cur);  // SENT state (the canonical confirms / corrects)
        UE_LOGI("laptop_buffer: local edit -> batch (%zu op(s), rwDelta=%d)",
                ops.size(), rwDelta);
    }
    // Send refused (backpressure): shadow stays; retry next poll, fresh seq.
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const uint64_t now = NowMs();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollMs;
    if (!L::EnsureResolved() || !L::Instance()) return;
    if (!g_announced) {
        g_announced = true;
        UE_LOGI("laptop_buffer: installed (quad lane; host=%d)", IsHost() ? 1 : 0);
    }

    g_asm.Sweep(Clock::now(), std::chrono::seconds(10));

    // A change of which disc the laptop holds is the slot lane's (coop/interactables/floppy_slot_sync), and it primes
    // this shadow as it takes one; until then both ends hold their sends.
    if (!g_havePrev) {
        L::BufferQuad q;
        if (L::ReadQuad(q)) PrimeFrom(q);  // first sight: silent prime (save state)
        return;
    }

    if (s->connected()) {
        if (IsHost()) {
            // Retry: a backpressure-refused canonical re-sends here.
            if (g_canonRetry) HostBroadcastCanonical(s);
            // Host organic: any quad change -> canonical on-change (no host
            // derivation exists -- R5-2). Int pre-filter first (R3): string
            // reads + hashing only when an int moved.
            if (!IntsUnchanged()) {
                L::BufferQuad cur;
                if (L::ReadQuad(cur)) {
                    if (QuadSeqHash(cur) != g_prevHash) {
                        UE_LOGI("laptop_buffer: host organic quad change -- canonical broadcast");
                        HostBroadcastCanonical(s);
                    } else {
                        PrimeFrom(cur);
                    }
                }
            }
        } else {
            DeriveAndSendLocal(s);
        }
    }
}

void OnQuadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread()) {
        UE_LOGW("laptop_buffer: OnQuadChunk off-game-thread -- dropping");
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    if (blob.empty()) return;
    if (!L::EnsureResolved() || !L::Instance()) {
        UE_LOGW("laptop_buffer: quad blob declined (laptop unresolved)");
        return;
    }
    Reader r{blob.data() + 1, blob.size() - 1};
    const uint8_t op = blob[0];
    const bool host = IsHost();
    if (op == 0 && host) {
        HostApplyBatch(r, senderSlot);
    } else if (op == 1 && !host) {
        if (senderSlot != 0) {
            UE_LOGW("laptop_buffer: canonical from non-host slot %u -- REJECTED",
                    static_cast<unsigned>(senderSlot));
            return;
        }
        ClientAdoptCanonical(r);
    }
}

Counts ReadCounts() { return g_counts; }

void PrimeQuadBaseline() {
    L::BufferQuad q;
    if (L::ReadQuad(q)) PrimeFrom(q);
}

void DevEjectAtNextBatch() { g_devEjectAtNextBatch = true; }

bool DevSendAppendOn(uint32_t generation, const std::wstring& row) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || IsHost() || !g_havePrev) return false;
    std::vector<std::wstring> after = g_prev.data;
    after.push_back(row);
    std::vector<EditOp> ops;
    DeriveArray(0, g_prev.data, after, nullptr, ops);
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::LaptopQuad, g_nextSeq++,
                                       PackBatch(ops, 0, generation));
}

bool DevSendCanonicalOn(uint32_t generation, const L::BufferQuad& q) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return false;
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::LaptopQuad, g_nextSeq++,
                                       PackCanonicalBounded(q, generation));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!L::EnsureResolved() || !L::Instance()) return;
    L::BufferQuad q;
    if (!L::ReadQuad(q)) return;
    if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::LaptopQuad, g_nextSeq++,
                                          PackCanonicalBounded(q, coop::floppy_slot_sync::LaptopGeneration()))) {
        UE_LOGI("laptop_buffer: connect canonical -> slot %d (fd=%zu fb=%zu rw=%d)",
                peerSlot, q.data.size(), q.buffer.size(), q.readWrites);
    } else {
        // Heal via the broadcast retry -- the joiner is ready, and a broadcast canonical
        // reaches it too.
        g_canonRetry = true;
        UE_LOGW("laptop_buffer: connect canonical -> slot %d REFUSED -- broadcast retry armed",
                peerSlot);
    }
}

void OnDisconnect() {
    if (g_counts.canonicalsTaken || g_counts.canonicalsDropped || g_counts.batchesRefused)
        UE_LOGI("laptop_buffer: session counts -- canonicals taken %llu, dropped %llu; batches refused %llu",
                static_cast<unsigned long long>(g_counts.canonicalsTaken),
                static_cast<unsigned long long>(g_counts.canonicalsDropped),
                static_cast<unsigned long long>(g_counts.batchesRefused));
    g_havePrev = false;
    g_prev = L::BufferQuad{};
    g_prevHash = 0;
    g_nextPoll = 0;
    g_asm.Clear();
    g_canonRetry = false;
    g_counts = Counts{};
    g_devEjectAtNextBatch = false;
    g_announced = false;
}

}  // namespace coop::laptop_buffer_sync
