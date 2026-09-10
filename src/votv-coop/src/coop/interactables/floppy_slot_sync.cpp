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
// slot [i32 type][i32 rw][u8 zip][u32 nametypeLen][utf8][u32 jsonLen][utf8][u16 rows]{[u32 len][utf8]}
// nametype is empty for every class but the laptop's, and it is on the wire because the format
// claims to leave room for that device: a member the digest raises an edge on and the wire drops
// is a field the receiver would blank on every change.

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

// An empty slot is a type and nothing else. What the device still holds in its other fields is
// residue its own eject left behind, so carrying it would put a stale disc's JSON on the wire for
// every empty box in the connect set.
void PackSlot(std::vector<uint8_t>& b, const Slot& s) {
    PutU32(b, static_cast<uint32_t>(s.st.floppyType));
    if (s.st.floppyType < 0) return;
    PutU32(b, static_cast<uint32_t>(s.st.readWrites));
    b.push_back(s.st.zip ? 1 : 0);
    PutStr32(b, s.c.nametype);
    PutStr32(b, s.c.objectData);
    // The count and the rows that follow it are the same number or the reader desyncs, so the
    // clamp binds both.
    const size_t rows = s.c.data.size() < 0xFFFF ? s.c.data.size() : 0xFFFF;
    PutU16(b, static_cast<uint16_t>(rows));
    for (size_t i = 0; i < rows; ++i) PutStr32(b, s.c.data[i]);
}

bool ParseSlot(Reader& r, Slot& out) {
    out.st.floppyType = static_cast<int32_t>(r.U32());
    if (!r.ok) return false;
    if (out.st.floppyType < 0) return true;  // empty: the type is the whole body
    out.st.readWrites = static_cast<int32_t>(r.U32());
    out.st.zip        = r.U8() != 0;
    out.c.nametype    = r.Str32();
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
std::set<uint32_t> g_unsendable;     // slots this peer cannot put on the wire at all

// CLIENT: until the host's canonical has landed, this peer's slots are its own save's, not the
// session's. Its boxes come up at class defaults, the save's loadData fills them a tick or two
// later, and a sweep that read THAT as a local edge would claim the joiner's stale world over the
// host's live one -- publishing, as canonical, the disappearance of a disc the host inserted after
// its last save. Prime and stay mute until the host has spoken.
bool g_haveCanonical = false;

// CLIENT: a claim the host drops -- for its rate, its size, a bad index or a malformed body --
// gets no answer of any kind, and this peer primed its shadow when the claim was SENT. Without
// this the divergence is permanent. Re-claim on a deadline, a bounded number of times.
struct Awaiting { uint64_t deadline = 0; int tries = 0; };
std::map<uint32_t, Awaiting> g_awaiting;
constexpr uint64_t kAnswerMs = 4000;
constexpr int      kMaxClaimTries = 3;

// HOST: a joiner whose connect set the transport refused has a permanently stale world -- the 1 Hz
// sweep covers edges, not a set nobody asked for. Re-send it.
std::map<int, int> g_connectRetry;   // peer slot -> tries left
constexpr int kMaxConnectTries = 5;
uint64_t g_nextSweep = 0;
coop::blob_chunks::Assembler g_asm;
uint32_t g_nextSeq = 1;

struct Rate { uint64_t windowStart = 0; int count = 0; bool warned = false; };
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

// A slot this peer cannot put on the wire -- past the size ceiling, or with rows the reader will
// not read. Prime the shadow so the sweep stops, drop the retry, and warn ONCE for this device.
// Leaving the shadow stale instead is a permanent 1 Hz re-read of the very field that is too big
// to read, with a log line every second; the slot is retried when it next changes, which is the
// only event that could make it sendable.
void ParkUnsendable(FS::DeviceKind kind, size_t index, void* device, const char* why,
                    size_t bytes) {
    const uint32_t key = ShadowKey(kind, index);
    if (g_unsendable.insert(key).second)
        UE_LOGW("floppy_slot_sync: device %zu's slot cannot travel (%s, %zu B) -- this peer keeps "
                "its own copy and the divergence is real; retried when the slot next changes",
                index, why, bytes);
    PrimeShadow(kind, index, device);
    g_retry.erase(key);
}

// A slot that packs within the ceiling, or nothing.
bool PackOne(uint8_t index, const Slot& cur, std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(index);
    PackSlot(out, cur);
    return out.size() <= kMaxSlotBytes;
}

// Pack a set of devices into canonical blobs, split at the slot cap so one huge slot cannot take
// the whole set down with it. Returns the blobs to send, in order.
std::vector<std::vector<uint8_t>> PackCanonicalSet(
    FS::DeviceKind kind, const std::vector<std::pair<uint8_t, Slot>>& entries,
    std::vector<uint8_t>* droppedOut = nullptr) {
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
                if (droppedOut) droppedOut->push_back(entries[i].first);
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
    // Two empty slots are the same slot. What each still holds in floppyReadwrites and
    // floppyObjectData is residue its own eject left for a deferred spawn to read, not state, and
    // comparing it would make every peer rewrite an empty box it already agrees about.
    if (a.st.floppyType < 0 || b.st.floppyType < 0)
        return a.st.floppyType < 0 && b.st.floppyType < 0;
    return a.st.floppyType == b.st.floppyType && a.st.readWrites == b.st.readWrites &&
           a.st.zip == b.st.zip && a.c.nametype == b.c.nametype &&
           a.c.objectData == b.c.objectData && a.c.data == b.c.data;
}

// Apply one slot to a live device and prime the shadow to what was written, so the next poll
// reads no edge. A slot that already reads the incoming value is left alone: at a join most of a
// base's boxes are empty on both peers, and writing each anyway would mint every string and swap
// every mesh in the frame that closes the connect set. Returns true when it wrote.
bool ApplySlot(FS::DeviceKind kind, size_t index, void* device, const Slot& s) {
    // Two empty slots agree before either one's content is read, and at a join most of a base's
    // boxes are that case -- reading the rows and the JSON first would mint a stale disc's record
    // per box for a comparison that never looks at it.
    FS::Scalars curSt{};
    if (FS::ReadScalars(kind, device, curSt) && curSt.floppyType < 0 && s.st.floppyType < 0) {
        PrimeShadow(kind, index, device);
        return false;
    }
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
    if (!ReadSlotOf(kind, device, cur)) {
        ParkUnsendable(kind, index, device, "its rows did not read", 0);
        return;
    }
    // No READY client -> prime silently. Publishing into the host's own world load only produces
    // refused chunks, and the joiner's ready edge sends the whole set anyway.
    if (!AnyClientReady(s)) {
        PrimeShadow(kind, index, device);
        g_retry.erase(ShadowKey(kind, index));
        return;
    }
    std::vector<uint8_t> body;
    if (!PackOne(static_cast<uint8_t>(index), cur, body)) {
        ParkUnsendable(kind, index, device, "past the size ceiling", body.size());
        return;
    }
    std::vector<uint8_t> blob;
    blob.push_back(kOpCanonical);
    blob.push_back(static_cast<uint8_t>(kind));
    PutU16(blob, 1);
    blob.insert(blob.end(), body.begin(), body.end());
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppySlotState, g_nextSeq++,
                                    blob)) {
        PrimeShadow(kind, index, device);
        g_retry.erase(ShadowKey(kind, index));
        g_unsendable.erase(ShadowKey(kind, index));
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
    if (!ReadSlotOf(kind, device, cur)) {
        ParkUnsendable(kind, index, device, "its rows did not read", 0);
        return;
    }
    std::vector<uint8_t> body;
    if (!PackOne(static_cast<uint8_t>(index), cur, body)) {
        ParkUnsendable(kind, index, device, "past the size ceiling", body.size());
        return;
    }
    std::vector<uint8_t> blob;
    blob.push_back(kOpClaim);
    blob.push_back(static_cast<uint8_t>(kind));
    blob.insert(blob.end(), body.begin(), body.end());
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppySlotState, g_nextSeq++,
                                    blob)) {
        // The claim is a report, not the truth: prime on the SENT state so the poll stops
        // re-claiming, and let the host's canonical correct it if the host said otherwise.
        PrimeShadow(kind, index, device);
        const uint32_t key = ShadowKey(kind, index);
        g_retry.erase(key);
        g_unsendable.erase(key);
        auto& aw = g_awaiting[key];
        if (aw.deadline == 0) aw.tries = 0;
        aw.deadline = NowMs() + kAnswerMs;
        UE_LOGI("floppy_slot_sync: CLIENT claim sent (device=%zu type=%d rw=%d rows=%zu json=%zu)",
                index, cur.st.floppyType, cur.st.readWrites, cur.c.data.size(),
                cur.c.objectData.size());
    } else {
        if (!g_retry.count(ShadowKey(kind, index)))
            UE_LOGW("floppy_slot_sync: claim for device %zu send refused -- retry armed", index);
        g_retry.insert(ShadowKey(kind, index));
    }
}

// Every device's slot to one joiner. True when every blob was accepted; a refused one leaves the
// retry armed, because the 1 Hz sweep covers EDGES and a set nobody asked for is not an edge.
bool SendConnectSet(coop::net::Session* s, int peerSlot) {
    const auto kind = FS::DeviceKind::ServerBox;
    if (!FS::EnsureResolved(kind)) return false;
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
    // Nothing to send is not the same as nothing to do: before the host's own world is up
    // ReadDevices answers zero, and treating that as a delivered set would leave the joiner
    // muted for the session -- it stays silent until a canonical lands.
    if (entries.empty()) return false;
    std::vector<uint8_t> dropped;
    auto blobs = PackCanonicalSet(kind, entries, &dropped);
    for (uint8_t idx : dropped)
        if (idx < n && devices[idx])
            ParkUnsendable(kind, idx, devices[idx], "past the size ceiling in the connect set", 0);
    int sent = 0;
    for (auto& b : blobs)
        if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::FloppySlotState,
                                              g_nextSeq++, b))
            ++sent;
    const bool whole = sent == static_cast<int>(blobs.size());
    UE_LOGI("floppy_slot_sync: connect set -> slot %d (%zu device(s) in %d of %zu blob(s))",
            peerSlot, entries.size(), sent, blobs.size());
    if (whole) g_connectRetry.erase(peerSlot);
    return whole;
}

// A joiner whose set the transport refused would otherwise keep a world it has never been told
// about: it primes its own slots and stays mute until one of them changes, which may be never.
void DrainConnectRetries(coop::net::Session* s) {
    if (g_connectRetry.empty()) return;
    std::vector<int> slots;
    for (const auto& kv : g_connectRetry) slots.push_back(kv.first);
    for (int slot : slots) {
        auto it = g_connectRetry.find(slot);
        if (it == g_connectRetry.end()) continue;
        if (!s->IsSlotWorldReady(slot)) { g_connectRetry.erase(it); continue; }
        if (--it->second <= 0) {
            UE_LOGW("floppy_slot_sync: connect set to slot %d refused %d times -- giving up; that "
                    "peer's device slots stay stale until one of them changes", slot,
                    kMaxConnectTries);
            g_connectRetry.erase(it);
            continue;
        }
        SendConnectSet(s, slot);
    }
}

// CLIENT: has the host answered the claim we sent for this slot? An answer is a canonical, and
// the host drops a claim silently for four different reasons, so a claim with no answer is
// re-sent a bounded number of times and then parked -- never left as a permanent divergence.
bool AnswerOverdue(uint32_t key, void* device, size_t index, FS::DeviceKind kind) {
    auto it = g_awaiting.find(key);
    if (it == g_awaiting.end()) return false;
    if (NowMs() < it->second.deadline) return false;
    if (it->second.tries >= kMaxClaimTries) {
        g_awaiting.erase(it);
        ParkUnsendable(kind, index, device, "the host never answered the claim", 0);
        return false;
    }
    ++it->second.tries;
    it->second.deadline = NowMs() + kAnswerMs;
    return true;
}

bool RateAllows(uint8_t senderSlot, bool& warnNow) {
    const uint64_t now = NowMs();
    Rate& r = g_rate[senderSlot];
    if (now - r.windowStart >= kRateWindowMs) { r.windowStart = now; r.count = 0; r.warned = false; }
    if (r.count >= kMaxClaimsPerWindow) {
        warnNow = !r.warned;
        r.warned = true;
        return false;
    }
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

    const bool host = IsHost();
    if (host) DrainConnectRetries(s);

    static std::vector<void*> devices;  // reused: the sweep must not allocate a list per second
    const size_t n = ReadDevices(kind, devices);
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
        if (!host && !g_haveCanonical) {
            // Pre-canonical, every local difference is this peer's own save loading. Prime it
            // away rather than claiming it.
            g_shadow[key] = digest;
            continue;
        }
        const bool overdue = !host && AnswerOverdue(key, d, i, kind);
        if (it->second == digest && !g_retry.count(key) && !overdue) continue;
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
        bool warnNow = false;
        if (!RateAllows(senderSlot, warnNow)) {
            if (warnNow)
                UE_LOGW("floppy_slot_sync: slot %u past %d claims per %llu ms -- dropping until "
                        "the window turns", static_cast<unsigned>(senderSlot),
                        kMaxClaimsPerWindow, static_cast<unsigned long long>(kRateWindowMs));
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
        // The type is the index the game's own eject spawns a disc class from, so a value its
        // list cannot answer for would have the box spawn from a null class and lose its
        // contents unrecoverably.
        if (!FS::IsSlotType(in.st.floppyType)) {
            UE_LOGW("floppy_slot_sync: claim from slot %u carries slot type %d, which no disc "
                    "class answers -- dropped", static_cast<unsigned>(senderSlot),
                    in.st.floppyType);
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
    if (count > kMaxDevices) {
        UE_LOGW("floppy_slot_sync: canonical names %u devices, past the %zu that can exist -- "
                "dropped", static_cast<unsigned>(count), kMaxDevices);
        return;
    }
    int applied = 0, skipped = 0, unchanged = 0;
    for (uint16_t k = 0; k < count && r.ok; ++k) {
        const uint8_t index = r.U8();
        Slot in;
        if (!ParseSlot(r, in)) break;
        if (index >= n || !devices[index] || !R::IsLive(devices[index])) { ++skipped; continue; }
        if (!FS::IsSlotType(in.st.floppyType)) { ++skipped; continue; }
        g_awaiting.erase(ShadowKey(kind, index));
        if (ApplySlot(kind, index, devices[index], in)) ++applied;
        else ++unchanged;
    }
    if (!r.ok)
        UE_LOGW("floppy_slot_sync: canonical truncated after %d of %u device(s)",
                applied + unchanged + skipped, count);
    g_haveCanonical = true;
    if (applied || skipped)
        UE_LOGI("floppy_slot_sync: CLIENT applied canonical -- %d device(s) written, %d already "
                "matched, %d skipped (of %zu local)", applied, unchanged, skipped, n);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    g_connectRetry[peerSlot] = kMaxConnectTries;
    SendConnectSet(s, peerSlot);
}

void OnPeerGone(uint8_t senderSlot) {
    g_asm.ClearSlot(senderSlot);
    g_rate.erase(senderSlot);
}

void OnDisconnect() {
    g_shadow.clear();
    g_retry.clear();
    g_unsendable.clear();
    g_awaiting.clear();
    g_connectRetry.clear();
    g_haveCanonical = false;
    FS::ResetCache();
    g_rate.clear();
    g_asm.Clear();
    g_nextSweep = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::floppy_slot_sync
