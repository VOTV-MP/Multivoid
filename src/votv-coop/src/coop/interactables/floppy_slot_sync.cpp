// coop/interactables/floppy_slot_sync.cpp -- see coop/interactables/floppy_slot_sync.h.

#include "coop/interactables/floppy_slot_sync.h"

#include "coop/comms/chat_feed.h"  // ToUtf8
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/devices/serverbox.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::floppy_slot_sync {
namespace {

namespace FS = ue_wrap::floppy_slot;
namespace SB = ue_wrap::serverbox;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kSweepMs = 1000;   // the server lane's own cadence

// The identity is the device's index in its kind's own list, and the list is the gamemode's
// servers[]: the level places the boxes, so both peers build the same order. The cap is
// serverbox_sync's, for the same reason -- one lane must not address a box the other cannot.
constexpr size_t kMaxDevices = 64;

// One slot body's ceiling. The measure that set it is the record lane's: a real disc record
// measured 11.3 KB, and an 8 KB guess refused it inside one run. A body past this is a claim we
// refuse and a canonical we cannot send, both loudly -- truncating instead would hand the game a
// JSON its own stringToSaveData cannot parse, which is worse than not sending.
constexpr size_t kMaxSlotBytes = 32 * 1024;

// A client's claim is unbounded input. The window is generous next to the 1 Hz poll that
// produces one, so a peer playing normally never reaches it.
constexpr uint64_t kRateWindowMs = 2000;
constexpr int      kMaxClaimsPerWindow = 12;

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

// ---- wire ----
// head [u8 op][u8 deviceKind]; op 0=claim{[u8 index][slot]}, 1=canonical{[u16 n]{[u8 index][slot]}}
// slot [i32 type][i32 rw][u8 zip][u32 jsonLen][utf8 json][u16 rows]{[u32 len][utf8]}

constexpr uint8_t kOpClaim     = 0;
constexpr uint8_t kOpCanonical = 1;

void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
// A row or a JSON can be long, so both take a 32-bit length; the reader bounds them against the
// blob it is reading out of.
void PutStr32(std::vector<uint8_t>& b, const std::wstring& w) {
    const std::string u = coop::chat_feed::ToUtf8(w);
    PutU32(b, static_cast<uint32_t>(u.size()));
    b.insert(b.end(), u.begin(), u.end());
}

struct Reader {
    const uint8_t* p; size_t n; size_t off = 0;
    bool ok = true;
    uint8_t U8() { if (off + 1 > n) { ok = false; return 0; } return p[off++]; }
    uint16_t U16() { if (off + 2 > n) { ok = false; return 0; } uint16_t v = static_cast<uint16_t>(p[off] | (p[off + 1] << 8)); off += 2; return v; }
    uint32_t U32() { if (off + 4 > n) { ok = false; return 0; } uint32_t v = 0; std::memcpy(&v, p + off, 4); off += 4; return v; }
    std::wstring Str32() {
        const uint32_t len = U32();
        if (!ok || len > n - off) { ok = false; return std::wstring(); }
        std::string u(reinterpret_cast<const char*>(p + off), len);
        off += len;
        return FromUtf8(u);
    }
};

struct Slot {
    FS::Scalars st;
    FS::Content c;
};

void PackSlot(std::vector<uint8_t>& b, const Slot& s) {
    PutU32(b, static_cast<uint32_t>(s.st.floppyType));
    PutU32(b, static_cast<uint32_t>(s.st.readWrites));
    b.push_back(s.st.zip ? 1 : 0);
    PutStr32(b, s.c.objectData);
    // The count and the rows that follow it are the same number or the reader desyncs, so the
    // clamp binds both.
    const size_t rows = s.c.data.size() < 0xFFFF ? s.c.data.size() : 0xFFFF;
    PutU16(b, static_cast<uint16_t>(rows));
    for (size_t i = 0; i < rows; ++i) PutStr32(b, s.c.data[i]);
}

bool ParseSlot(Reader& r, Slot& out) {
    out.st.floppyType = static_cast<int32_t>(r.U32());
    out.st.readWrites = static_cast<int32_t>(r.U32());
    out.st.zip        = r.U8() != 0;
    out.c.objectData  = r.Str32();
    const uint16_t rows = r.U16();
    for (uint16_t i = 0; i < rows && r.ok; ++i) out.c.data.push_back(r.Str32());
    return r.ok;
}

// ---- devices ----

// The live device list for a kind. Only the server box has one today; the laptop is the second
// row this wire format leaves room for, and its own lane still owns it.
size_t ReadDevices(FS::DeviceKind kind, std::vector<void*>& out) {
    out.clear();
    if (kind != FS::DeviceKind::ServerBox) return 0;
    SB::ReadServers(out);
    if (out.size() > kMaxDevices) out.resize(kMaxDevices);
    return out.size();
}

bool ReadSlotOf(FS::DeviceKind kind, void* device, Slot& out) {
    return FS::ReadScalars(kind, device, out.st) && FS::ReadContent(kind, device, out.c);
}

// ---- state ----

// Per (kind, index): the digest of the slot as this peer last published or applied it. The poll
// compares against it, so a wire apply can never be read back as a local edit.
std::map<uint32_t, uint64_t> g_shadow;
std::set<uint32_t> g_retry;          // sends the transport refused
uint64_t g_nextSweep = 0;
coop::blob_chunks::Assembler g_asm;
uint32_t g_nextSeq = 1;

struct Rate { uint64_t windowStart = 0; int count = 0; };
std::map<uint8_t, Rate> g_rate;      // per sender slot

uint32_t ShadowKey(FS::DeviceKind kind, size_t index) {
    return (static_cast<uint32_t>(kind) << 16) | static_cast<uint32_t>(index);
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

bool AnyClientReady(coop::net::Session* s) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
        if (s->IsSlotWorldReady(slot)) return true;
    return false;
}

void PrimeShadow(FS::DeviceKind kind, size_t index, void* device) {
    uint64_t d = 0;
    if (FS::ReadDigest(kind, device, d)) g_shadow[ShadowKey(kind, index)] = d;
}

// Pack a set of devices into canonical blobs, split at the slot cap so one huge slot cannot take
// the whole set down with it. Returns the blobs to send, in order.
std::vector<std::vector<uint8_t>> PackCanonicalSet(
    FS::DeviceKind kind, const std::vector<std::pair<uint8_t, Slot>>& entries) {
    std::vector<std::vector<uint8_t>> blobs;
    size_t i = 0;
    while (i < entries.size()) {
        std::vector<uint8_t> body;
        uint16_t count = 0;
        while (i < entries.size()) {
            std::vector<uint8_t> one;
            one.push_back(entries[i].first);
            PackSlot(one, entries[i].second);
            if (one.size() > kMaxSlotBytes) {
                UE_LOGW("floppy_slot_sync: device %u's slot is %zu B, past the %zu B ceiling -- "
                        "NOT sent; that peer keeps whatever it has and the divergence is real",
                        static_cast<unsigned>(entries[i].first), one.size(), kMaxSlotBytes);
                ++i;
                continue;
            }
            // 4 = the head plus the count; keep a whole entry together.
            if (count > 0 && body.size() + one.size() + 4 > coop::blob_chunks::MaxBlobBytes())
                break;
            body.insert(body.end(), one.begin(), one.end());
            ++count;
            ++i;
        }
        if (count == 0) continue;
        std::vector<uint8_t> blob;
        blob.push_back(kOpCanonical);
        blob.push_back(static_cast<uint8_t>(kind));
        PutU16(blob, count);
        blob.insert(blob.end(), body.begin(), body.end());
        blobs.push_back(std::move(blob));
    }
    return blobs;
}

bool SlotEquals(const Slot& a, const Slot& b) {
    return a.st.floppyType == b.st.floppyType && a.st.readWrites == b.st.readWrites &&
           a.st.zip == b.st.zip && a.c.objectData == b.c.objectData && a.c.data == b.c.data;
}

// Apply one slot to a live device and prime the shadow to what was written, so the next poll
// reads no edge. A slot that already reads the incoming value is left alone: at a join most of a
// base's boxes are empty on both peers, and writing each anyway would mint every string and swap
// every mesh in the frame that closes the connect set. Returns true when it wrote.
bool ApplySlot(FS::DeviceKind kind, size_t index, void* device, const Slot& s) {
    Slot cur;
    const bool read = ReadSlotOf(kind, device, cur);
    if (read && SlotEquals(cur, s)) {
        PrimeShadow(kind, index, device);
        return false;
    }
    if (s.st.floppyType < 0) FS::ClearSlot(kind, device);
    else                     FS::WriteSlot(kind, device, s.st, s.c);
    PrimeShadow(kind, index, device);
    return true;
}

// ---- host ----

void HostBroadcastOne(coop::net::Session* s, FS::DeviceKind kind, size_t index, void* device) {
    Slot cur;
    if (!ReadSlotOf(kind, device, cur)) return;
    // No READY client -> prime silently. Publishing into the host's own world load only produces
    // refused chunks, and the joiner's ready edge sends the whole set anyway.
    if (!AnyClientReady(s)) {
        PrimeShadow(kind, index, device);
        g_retry.erase(ShadowKey(kind, index));
        return;
    }
    std::vector<std::pair<uint8_t, Slot>> one{{static_cast<uint8_t>(index), cur}};
    auto blobs = PackCanonicalSet(kind, one);
    bool sent = !blobs.empty();
    for (auto& b : blobs)
        sent = coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppySlotState,
                                           g_nextSeq++, b) && sent;
    if (sent) {
        PrimeShadow(kind, index, device);
        g_retry.erase(ShadowKey(kind, index));
    } else {
        if (!g_retry.count(ShadowKey(kind, index)))
            UE_LOGW("floppy_slot_sync: canonical for device %zu send refused -- retry armed "
                    "(1 Hz sweep)", index);
        g_retry.insert(ShadowKey(kind, index));
    }
}

// ---- client ----

void ClientClaim(coop::net::Session* s, FS::DeviceKind kind, size_t index, void* device) {
    Slot cur;
    if (!ReadSlotOf(kind, device, cur)) return;
    std::vector<uint8_t> blob;
    blob.push_back(kOpClaim);
    blob.push_back(static_cast<uint8_t>(kind));
    blob.push_back(static_cast<uint8_t>(index));
    PackSlot(blob, cur);
    if (blob.size() > kMaxSlotBytes) {
        UE_LOGW("floppy_slot_sync: local slot %zu is %zu B, past the %zu B ceiling -- claim NOT "
                "sent; the host keeps its own copy of this slot", index, blob.size(),
                kMaxSlotBytes);
        PrimeShadow(kind, index, device);  // do not re-warn every sweep for a slot that cannot travel
        return;
    }
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppySlotState, g_nextSeq++,
                                    blob)) {
        // The claim is a report, not the truth: prime on the SENT state so the poll stops
        // re-claiming, and let the host's canonical correct it if the host said otherwise.
        PrimeShadow(kind, index, device);
        g_retry.erase(ShadowKey(kind, index));
        UE_LOGI("floppy_slot_sync: CLIENT claim sent (device=%zu type=%d rw=%d rows=%zu json=%zu)",
                index, cur.st.floppyType, cur.st.readWrites, cur.c.data.size(),
                cur.c.objectData.size());
    } else {
        if (!g_retry.count(ShadowKey(kind, index)))
            UE_LOGW("floppy_slot_sync: claim for device %zu send refused -- retry armed", index);
        g_retry.insert(ShadowKey(kind, index));
    }
}

bool RateAllows(uint8_t senderSlot) {
    const uint64_t now = NowMs();
    Rate& r = g_rate[senderSlot];
    if (now - r.windowStart >= kRateWindowMs) { r.windowStart = now; r.count = 0; }
    if (r.count >= kMaxClaimsPerWindow) return false;
    ++r.count;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const uint64_t now = NowMs();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));

    const auto kind = FS::DeviceKind::ServerBox;
    if (!FS::EnsureResolved(kind)) return;

    static std::vector<void*> devices;  // reused: the sweep must not allocate a list per second
    const size_t n = ReadDevices(kind, devices);
    const bool host = IsHost();
    for (size_t i = 0; i < n; ++i) {
        void* d = devices[i];
        if (!d || !R::IsLive(d)) continue;
        uint64_t digest = 0;
        if (!FS::ReadDigest(kind, d, digest)) continue;
        const uint32_t key = ShadowKey(kind, i);
        auto it = g_shadow.find(key);
        if (it == g_shadow.end()) {
            // First sight primes silently: a prime is not an edge, and the joiner's own connect
            // set is what makes the two peers agree at the start.
            g_shadow[key] = digest;
            continue;
        }
        if (it->second == digest && !g_retry.count(key)) continue;
        if (host) HostBroadcastOne(s, kind, i, d);
        else      ClientClaim(s, kind, i, d);
    }
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread()) {
        UE_LOGW("floppy_slot_sync: OnChunk off-game-thread -- dropping");
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    if (blob.size() < 2) return;

    Reader r{blob.data(), blob.size()};
    const uint8_t op   = r.U8();
    const uint8_t kindB = r.U8();
    if (kindB >= ue_wrap::floppy_slot::kDeviceKindCount) {
        UE_LOGW("floppy_slot_sync: unknown device kind %u from slot %u -- dropped",
                static_cast<unsigned>(kindB), static_cast<unsigned>(senderSlot));
        return;
    }
    const auto kind = static_cast<FS::DeviceKind>(kindB);
    if (!FS::EnsureResolved(kind)) {
        UE_LOGW("floppy_slot_sync: chunk arrived before %u resolved -- dropped (the 1 Hz sweep "
                "and the connect set re-deliver)", static_cast<unsigned>(kindB));
        return;
    }
    std::vector<void*> devices;
    const size_t n = ReadDevices(kind, devices);
    const bool host = IsHost();

    if (op == kOpClaim) {
        if (!host) return;  // a client never takes another peer's claim; only the host's canonical
        if (blob.size() > kMaxSlotBytes) {
            UE_LOGW("floppy_slot_sync: claim from slot %u is %zu B, past the %zu B ceiling -- "
                    "dropped", static_cast<unsigned>(senderSlot), blob.size(), kMaxSlotBytes);
            return;
        }
        if (!RateAllows(senderSlot)) {
            UE_LOGW("floppy_slot_sync: slot %u past %d claims per %llu ms -- claim dropped",
                    static_cast<unsigned>(senderSlot), kMaxClaimsPerWindow,
                    static_cast<unsigned long long>(kRateWindowMs));
            return;
        }
        const uint8_t index = r.U8();
        Slot in;
        if (!ParseSlot(r, in)) {
            UE_LOGW("floppy_slot_sync: malformed claim from slot %u -- dropped",
                    static_cast<unsigned>(senderSlot));
            return;
        }
        if (index >= n || !devices[index] || !R::IsLive(devices[index])) {
            UE_LOGW("floppy_slot_sync: claim from slot %u names device %u of %zu -- dropped",
                    static_cast<unsigned>(senderSlot), static_cast<unsigned>(index), n);
            return;
        }
        ApplySlot(kind, index, devices[index], in);
        UE_LOGI("floppy_slot_sync: HOST took slot claim (device=%u type=%d rw=%d rows=%zu) from "
                "slot %u -- answering with the canonical", static_cast<unsigned>(index),
                in.st.floppyType, in.st.readWrites, in.c.data.size(),
                static_cast<unsigned>(senderSlot));
        // The re-publish IS the acknowledgement, and it goes to the author too: if the host's own
        // copy differs from what the claim said, the author is the peer that most needs correcting.
        HostBroadcastOne(s, kind, index, devices[index]);
        return;
    }

    if (op != kOpCanonical) return;
    if (host) {
        UE_LOGW("floppy_slot_sync: canonical received on the HOST from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (senderSlot != 0) {
        UE_LOGW("floppy_slot_sync: canonical from non-host slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    const uint16_t count = r.U16();
    int applied = 0, skipped = 0, unchanged = 0;
    for (uint16_t k = 0; k < count && r.ok; ++k) {
        const uint8_t index = r.U8();
        Slot in;
        if (!ParseSlot(r, in)) break;
        if (index >= n || !devices[index] || !R::IsLive(devices[index])) { ++skipped; continue; }
        if (ApplySlot(kind, index, devices[index], in)) ++applied;
        else ++unchanged;
    }
    if (!r.ok)
        UE_LOGW("floppy_slot_sync: canonical truncated after %d of %u device(s)", applied, count);
    if (applied || skipped)
        UE_LOGI("floppy_slot_sync: CLIENT applied canonical -- %d device(s) written, %d already "
                "matched, %d skipped (of %zu local)", applied, unchanged, skipped, n);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    const auto kind = FS::DeviceKind::ServerBox;
    if (!FS::EnsureResolved(kind)) return;
    std::vector<void*> devices;
    const size_t n = ReadDevices(kind, devices);
    std::vector<std::pair<uint8_t, Slot>> entries;
    for (size_t i = 0; i < n; ++i) {
        void* d = devices[i];
        if (!d || !R::IsLive(d)) continue;
        Slot cur;
        if (!ReadSlotOf(kind, d, cur)) continue;
        // No prime here: the shadow tracks what the host last BROADCAST, and this set goes to one
        // joiner. Priming it would swallow a change the already-connected peers have not had yet.
        entries.emplace_back(static_cast<uint8_t>(i), std::move(cur));
    }
    if (entries.empty()) return;
    auto blobs = PackCanonicalSet(kind, entries);
    int sent = 0;
    for (auto& b : blobs)
        if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::FloppySlotState,
                                              g_nextSeq++, b))
            ++sent;
    UE_LOGI("floppy_slot_sync: connect set -> slot %d (%zu device(s) in %d of %zu blob(s))",
            peerSlot, entries.size(), sent, blobs.size());
    if (sent != static_cast<int>(blobs.size()))
        UE_LOGW("floppy_slot_sync: connect set to slot %d sent %d of %zu blob(s) -- the 1 Hz "
                "sweep does not cover a joiner's missing set, so those slots stay stale until "
                "one of them next changes", peerSlot, sent, blobs.size());
}

void OnPeerGone(uint8_t senderSlot) {
    g_asm.ClearSlot(senderSlot);
    g_rate.erase(senderSlot);
}

void OnDisconnect() {
    g_shadow.clear();
    g_retry.clear();
    g_rate.clear();
    g_asm.Clear();
    g_nextSweep = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::floppy_slot_sync
