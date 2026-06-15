// coop/player_inventory_sync.cpp -- see coop/player_inventory_sync.h.

#include "coop/player_inventory_sync.h"

#include "coop/blob_chunks.h"
#include "coop/ini_config.h"
#include "coop/inventory_wire.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player_handshake.h"
#include "coop/save_guard.h"
#include "coop/save_transfer.h"
#include "ue_wrap/engine.h"      // Inc 4: SetSaveObjectReadyHook -- the pre-materialize apply point
#include "ue_wrap/inventory.h"
#include "ue_wrap/log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace coop::player_inventory_sync {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- HOST: per-slot received-blob state + the reassembler ----
coop::blob_chunks::Assembler g_assembler;
struct HostEntry {
    std::string          guid;
    std::string          nick;                // SNAPSHOT at receive (game thread); FlushSlot must
                                              // NOT call the GT-only NicknameForSlot, because
                                              // FlushAllToDisk runs on the WINDOW thread at shutdown
    std::vector<uint8_t> blob;
    uint64_t             hash = 0;
    bool                 dirty = false;       // received, not yet flushed to disk
    Clock::time_point    lastWrite{};         // 15 s rate-limit
};
std::array<HostEntry, coop::net::kMaxPeers> g_hostBySlot;

// ---- CLIENT: send-dedup state ----
uint64_t          g_lastSentHash = 0;
uint32_t          g_sendSeq = 0;
Clock::time_point g_lastPoll{};
Clock::time_point g_lastSweep{};

constexpr auto kClientPoll  = std::chrono::seconds(1);
constexpr auto kWriteRate   = std::chrono::seconds(15);
constexpr auto kAsmTtl      = std::chrono::seconds(30);

// ---- INCREMENT 4: the live apply on join (host->client + the SaveObjectReadyHook) ----
// Staged-rollout gate (ini `inventory_apply`, default OFF): when OFF, the host->client send is
// still exercised but the CLIENT never writes the save object (the v56 whole-host-save inventory
// inheritance stays in effect). When ON, the per-player inventory is applied on join, RETIRING
// the v56 inheritance (a transitional gate with a written retirement plan -- see the impl-plan
// doc + the findings doc; flip to default-ON once the inventory_probe + hands-on confirm
// items-usable). Read once.
bool g_applyEnabledInit = false;
bool g_applyEnabled = false;
bool ApplyEnabled() {
    if (!g_applyEnabledInit) {
        g_applyEnabled = ::coop::ini_config::IsIniKeyTrue("inventory_apply");
        g_applyEnabledInit = true;
    }
    return g_applyEnabled;
}

// CLIENT: the host's on-join apply blob, reassembled here (separate Assembler from the host's
// receive path -- host->client is a distinct stream direction). Deserialized into g_pendingApply.
coop::blob_chunks::Assembler g_clientAssembler;
ue_wrap::inventory::PlayerInventory g_pendingApply;
std::atomic<bool> g_hasPendingApply{false};

// HOST: per-slot send sequence for the on-join inventory push (independent of the client stream's
// g_sendSeq space; assembler keys are per-(sender,seq) so they never collide cross-direction).
std::array<uint32_t, coop::net::kMaxPeers> g_hostSendSeq{};
// HOST: per-slot "apply blob already pushed this connection" latch. The push must land in the
// joiner's PRE-WORLD window (the ConnectReplayForSlot edge fires at world-ready, too late), so it
// is driven from the host tick the instant the slot is connected AND its Join GUID has arrived --
// which is well before the client finishes its save transfer + loads. Reset on disconnect.
std::array<bool, coop::net::kMaxPeers> g_applySentToSlot{};

// Build <SaveGames>/<hostSlot>/coop_players/<guid>.json. Empty path if any piece is missing.
fs::path PlayerFilePath(const std::string& guid) {
    // Defense in depth (the wire boundary already validates): a GUID that is not exactly 32 hex
    // chars must NEVER become a path component -- return empty so every write path no-ops cleanly,
    // foreclosing path traversal even if a non-hex guid ever reaches here. (Adversarial-verify HIGH.)
    if (!coop::player_handshake::IsValidGuid(guid)) return {};
    const fs::path base = coop::save_guard::SaveGamesDir();
    if (base.empty()) return {};
    const std::wstring slot = coop::save_transfer::HostSlot();
    if (slot.empty()) return {};
    return base / slot / L"coop_players" / (std::wstring(guid.begin(), guid.end()) + L".json");
}

std::string Hex(const std::vector<uint8_t>& b) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s.push_back(k[c >> 4]); s.push_back(k[c & 0xF]); }
    return s;
}

std::string NarrowNick(const std::wstring& w) {
    std::string s;                       // nick is already SanitizeNickname'd (ASCII alnum + [-_. ])
    s.reserve(w.size());
    for (wchar_t c : w) if (c >= 32 && c < 127 && c != '"' && c != '\\') s.push_back(static_cast<char>(c));
    return s;
}

// Persist `blob` to `file` ROBUSTLY: magic + FNV integrity + a readable nick/lastSeen, written
// atomically (tmp + rename) and keeping a .bak of the last-good file so a corrupt edit can be
// recovered (the user-tinkering case). Returns false on I/O failure (logged).
bool WriteBlobFile(const fs::path& file, const std::vector<uint8_t>& blob, const std::string& nick) {
    if (file.empty()) return false;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        UE_LOGW("player_inventory: create_directories('%ls') failed: %s",
                file.parent_path().c_str(), ec.message().c_str());
        return false;
    }
    // Keep the last-good file as <file>.bak before overwriting (corruption recovery).
    if (fs::exists(file, ec))
        fs::copy_file(file, fs::path(file).concat(L".bak"), fs::copy_options::overwrite_existing, ec);
    const uint64_t fnv = coop::blob_chunks::Fnv64(blob);
    const long long epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char fnvhex[17] = {};
    std::snprintf(fnvhex, sizeof(fnvhex), "%016llx", static_cast<unsigned long long>(fnv));
    std::string json = "{\"magic\":\"VCPI\",\"ver\":1,\"fnv\":\"";
    json += fnvhex;
    json += "\",\"nick\":\"";
    json += nick;
    json += "\",\"lastSeen\":";
    json += std::to_string(epoch);
    json += ",\"blob\":\"";
    json += Hex(blob);
    json += "\"}\n";
    const fs::path tmp = fs::path(file).concat(L".part");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << json)) {
            UE_LOGE("player_inventory: write failed ('%ls')", tmp.c_str());
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        UE_LOGE("player_inventory: rename('%ls') failed: %s", file.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// Decode a lowercase/upper hex string to bytes. False on an odd length or a non-hex digit.
bool UnHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() & 1) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Extract the value of a "key":"..." string field from our flat JSON (no nesting/escapes used).
bool JsonStr(const std::string& doc, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    const size_t k = doc.find(needle);
    if (k == std::string::npos) return false;
    const size_t v = k + needle.size();
    const size_t e = doc.find('"', v);
    if (e == std::string::npos) return false;
    out = doc.substr(v, e - v);
    return true;
}

// Parse one persisted inventory file at `file` into `outBlob`. Defensive against host tinkering
// (the user-edit case): requires magic "VCPI", a parseable hex blob, and a matching FNV (the
// blob's stored integrity hash). False (caller tries .bak, then EMPTY) on any failure -- never
// throws, never returns unverified bytes.
bool ParseBlobFile(const fs::path& file, std::vector<uint8_t>& outBlob) {
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string doc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string magic;
    if (!JsonStr(doc, "magic", magic) || magic != "VCPI") {
        UE_LOGW("player_inventory: '%ls' missing/bad magic -- treating as corrupt", file.c_str());
        return false;
    }
    std::string blobHex, fnvHex;
    if (!JsonStr(doc, "blob", blobHex) || !JsonStr(doc, "fnv", fnvHex)) return false;
    std::vector<uint8_t> blob;
    if (!UnHex(blobHex, blob)) {
        UE_LOGW("player_inventory: '%ls' blob hex unparseable -- corrupt", file.c_str());
        return false;
    }
    char want[17] = {};
    std::snprintf(want, sizeof(want), "%016llx",
                  static_cast<unsigned long long>(coop::blob_chunks::Fnv64(blob)));
    if (fnvHex != want) {
        UE_LOGW("player_inventory: '%ls' FNV mismatch (have %s, stored %s) -- corrupt/tampered",
                file.c_str(), want, fnvHex.c_str());
        return false;
    }
    outBlob = std::move(blob);
    return true;
}

// Read peer GUID `guid`'s persisted inventory blob, FNV-verified, with a .bak fallback. Returns
// false (caller uses an EMPTY inventory) when neither the file nor its .bak is valid -- the
// fail-safe that guarantees a corrupt/missing file can NEVER leak another player's inventory or
// crash; the player just starts empty.
bool ReadBlobFile(const std::string& guid, std::vector<uint8_t>& outBlob) {
    const fs::path file = PlayerFilePath(guid);
    if (file.empty()) return false;
    if (ParseBlobFile(file, outBlob)) return true;
    const fs::path bak = fs::path(file).concat(L".bak");
    if (ParseBlobFile(bak, outBlob)) {
        UE_LOGW("player_inventory: recovered guid=%s inventory from .bak (primary corrupt)",
                guid.c_str());
        return true;
    }
    return false;
}

// Flush one host slot's pending blob to its <guid>.json (ignores the rate-limit -- used on
// disconnect/shutdown). Clears dirty.
void FlushSlot(int slot) {
    if (slot < 0 || slot >= coop::net::kMaxPeers) return;
    HostEntry& e = g_hostBySlot[slot];
    if (!e.dirty || e.guid.empty()) return;
    // Use the nick SNAPSHOT captured in OnReliable (game thread). FlushSlot is reachable from the
    // window-thread shutdown flush (FlushAllToDisk), so it must touch NO game-thread-only state
    // (NicknameForSlot reads a GT-only side table -- a torn-wstring race on host exit otherwise).
    if (WriteBlobFile(PlayerFilePath(e.guid), e.blob, e.nick)) {
        e.dirty = false;
        e.lastWrite = Clock::now();
        UE_LOGI("player_inventory: flushed slot %d guid=%s (%zu-byte blob) to disk",
                slot, e.guid.c_str(), e.blob.size());
    }
}

// CLIENT: poll the live inventory, serialize, and stream to the host ON CHANGE (~1 Hz).
void ClientStreamTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastPoll < kClientPoll) return;
    g_lastPoll = now;
    ue_wrap::inventory::PlayerInventory inv;
    if (!ue_wrap::inventory::ReadAll(inv)) return;  // saveSlot not up yet
    const std::vector<uint8_t> blob = coop::inventory_wire::Serialize(inv);
    const uint64_t hash = coop::blob_chunks::Fnv64(blob);
    if (hash == g_lastSentHash) return;  // unchanged -> don't re-send
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::PlayerInventoryBlob, ++g_sendSeq, blob)) {
        g_lastSentHash = hash;
        UE_LOGI("player_inventory[client]: streamed inventory blob (%zu bytes, %zu items) to host",
                blob.size(), inv.inventory.size());
    }  // else: channel busy -> retry next poll under a fresh seq (the blob unchanged)
}

// HOST: sweep stale half-assemblies + flush rate-limited dirty blobs + push each newly-connected
// joiner its per-player apply blob ONCE (the pre-world connect edge -- see g_applySentToSlot).
void HostPersistTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastSweep < std::chrono::seconds(1)) return;
    g_lastSweep = now;
    g_assembler.Sweep(now, kAsmTtl);
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        HostEntry& e = g_hostBySlot[slot];
        if (e.dirty && now - e.lastWrite >= kWriteRate) FlushSlot(slot);
        // Connect-edge apply push: connected + GUID arrived + not yet sent -> send once.
        if (!g_applySentToSlot[slot] && s->IsSlotConnected(slot) &&
            !coop::player_handshake::GuidForSlot(slot).empty()) {
            SendInventoryToSlot(slot);
            g_applySentToSlot[slot] = true;
        }
    }
}

// CLIENT: the engine's pre-materialize hook (registered in Install). Fires on the game thread
// with the freshly loaded/created save object, BEFORE the native loadObjects() builds the world
// from it -- the one window to substitute this client's per-player inventory. Self-gates: only a
// CLIENT, only with the apply gate ON, only with a pending blob (the join boot waits for it). On
// a miss it SKIPS (leaves the loaded inventory) rather than wiping -- never destroys data it
// can't replace.
void OnSaveObjectReady(void* saveSlotObject) {
    if (!ApplyEnabled()) return;                    // staged-rollout gate (v56 inheritance stays)
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;  // only a joining client applies
    if (!g_hasPendingApply.load(std::memory_order_acquire)) {
        UE_LOGW("player_inventory: SaveObjectReady but no apply blob arrived yet -- leaving the "
                "loaded inventory (NOT wiping). The join boot should have waited for it.");
        return;
    }
    if (ue_wrap::inventory::ApplyToSaveObject(saveSlotObject, g_pendingApply)) {
        UE_LOGI("player_inventory[client]: applied per-player inventory to save object %p "
                "(inventory=%zu equip=%zu hold=%zu) -- RETIRES the v56 host-save inheritance",
                saveSlotObject, g_pendingApply.inventory.size(),
                g_pendingApply.equipment.size(), g_pendingApply.hold.size());
    } else {
        UE_LOGE("player_inventory[client]: ApplyToSaveObject FAILED on %p -- the client will "
                "fall back to the v56-loaded inventory (no wipe)", saveSlotObject);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Inc 4: arm the engine's pre-materialize apply point (no-op until a client join with the
    // apply gate on + a pending blob). Idempotent re-arm across Stop()/Start() is harmless.
    ue_wrap::engine::SetSaveObjectReadyHook(&OnSaveObjectReady);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected()) {
        if (s->role() == coop::net::Role::Client) ClientStreamTick(s);
        else                                      HostPersistTick(s);
    }

    // INCREMENT 2 read-verify self-test (ini inventory_selftest=1). One-shot; dev diagnostic.
    static const bool s_selftest = ::coop::ini_config::IsIniKeyTrue("inventory_selftest");
    if (!s_selftest) return;
    static bool s_done = false;
    if (s_done) return;
    static int s_ticks = 0;
    if (++s_ticks < 600) return;  // ~5s settle (world up + saveSlot resolvable)
    ue_wrap::inventory::PlayerInventory inv;
    if (!ue_wrap::inventory::ReadAll(inv)) return;  // saveSlot not up yet -> retry next tick
    s_done = true;
    UE_LOGI("inventory[selftest]: read local saveSlot -- inventory=%zu equipment=%zu hold=%zu",
            inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    const std::vector<uint8_t> blob1 = coop::inventory_wire::Serialize(inv);
    ue_wrap::inventory::PlayerInventory inv2;
    const bool de = coop::inventory_wire::Deserialize(blob1, inv2);
    std::vector<uint8_t> blob2;
    if (de) blob2 = coop::inventory_wire::Serialize(inv2);
    const bool roundtrip = de && blob1 == blob2;
    UE_LOGI("inventory[selftest]: serialize=%zu bytes, deserialize=%d, ROUND-TRIP %s "
            "(inv2: inventory=%zu equip=%zu hold=%zu)",
            blob1.size(), de ? 1 : 0, roundtrip ? "OK" : "MISMATCH",
            inv2.inventory.size(), inv2.equipment.size(), inv2.hold.size());
}

void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;

    // CLIENT direction (Inc 4): the host (slot 0) pushed this client its per-player apply blob.
    if (s->role() == coop::net::Role::Client) {
        if (senderPeerSlot != 0) return;  // only the host pushes the apply blob
        std::vector<uint8_t> blob;
        if (!g_clientAssembler.OnChunk(p, senderPeerSlot, blob)) return;  // not complete yet
        ue_wrap::inventory::PlayerInventory inv;
        if (!coop::inventory_wire::Deserialize(blob, inv)) {
            UE_LOGW("player_inventory[client]: host apply blob (%zu bytes) failed to deserialize "
                    "-- ignoring (will keep waiting / fall back)", blob.size());
            return;
        }
        g_pendingApply = std::move(inv);
        g_hasPendingApply.store(true, std::memory_order_release);
        UE_LOGI("player_inventory[client]: received per-player apply blob from host (%zu bytes, "
                "inventory=%zu equip=%zu hold=%zu)", blob.size(), g_pendingApply.inventory.size(),
                g_pendingApply.equipment.size(), g_pendingApply.hold.size());
        return;
    }

    // HOST direction (Inc 3): a CLIENT slot streamed its live inventory to persist.
    if (s->role() != coop::net::Role::Host) return;
    if (senderPeerSlot < 1 || senderPeerSlot >= coop::net::kMaxPeers) return;  // a CLIENT slot
    std::vector<uint8_t> blob;
    if (!g_assembler.OnChunk(p, senderPeerSlot, blob)) return;  // not complete yet
    const std::string& guid = coop::player_handshake::GuidForSlot(senderPeerSlot);
    if (guid.empty()) {
        UE_LOGW("player_inventory: inventory blob from slot %u but no GUID -- dropping",
                senderPeerSlot);
        return;
    }
    const uint64_t hash = coop::blob_chunks::Fnv64(blob);
    HostEntry& e = g_hostBySlot[senderPeerSlot];
    if (e.hash == hash && !e.guid.empty()) return;  // unchanged (dedup)
    e.guid = guid;
    e.blob = std::move(blob);
    e.hash = hash;
    e.nick = NarrowNick(coop::player_handshake::NicknameForSlot(senderPeerSlot));  // GT snapshot
    e.dirty = true;
    // Write now if the rate-limit window has passed; else HostPersistTick flushes it.
    if (Clock::now() - e.lastWrite >= kWriteRate) FlushSlot(senderPeerSlot);
}

void SendInventoryToSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host pushes; clients receive
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    const std::string& guid = coop::player_handshake::GuidForSlot(peerSlot);
    if (guid.empty()) {
        UE_LOGI("player_inventory: slot %d has no GUID yet -- not sending an apply blob this edge",
                peerSlot);
        return;
    }
    std::vector<uint8_t> blob;
    if (!ReadBlobFile(guid, blob)) {
        // No file / corrupt + no .bak -> send an EMPTY inventory (fail-safe; the client starts
        // empty, NEVER inheriting the host's or another player's items).
        blob = coop::inventory_wire::Serialize(ue_wrap::inventory::PlayerInventory{});
        UE_LOGI("player_inventory: slot %d guid=%s -- no valid saved inventory; sending EMPTY",
                peerSlot, guid.c_str());
    }
    if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::PlayerInventoryBlob,
                                          ++g_hostSendSeq[peerSlot], blob)) {
        UE_LOGI("player_inventory: sent slot %d guid=%s its per-player inventory (%zu-byte blob)",
                peerSlot, guid.c_str(), blob.size());
    } else {
        UE_LOGW("player_inventory: send to slot %d refused (channel busy) -- the join boot's "
                "wait-for-apply will retry the edge", peerSlot);
    }
}

bool HasPendingApply() { return g_hasPendingApply.load(std::memory_order_acquire); }

bool IsApplyEnabled() { return ApplyEnabled(); }

void OnDisconnectForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    FlushSlot(peerSlot);                 // last authoritative state to disk
    if (peerSlot >= 0 && peerSlot < coop::net::kMaxPeers) {
        g_hostBySlot[peerSlot] = HostEntry{};
        g_applySentToSlot[peerSlot] = false;  // a rejoin re-pushes the apply blob
    }
}

void OnDisconnect() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) {
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) FlushSlot(slot);
    }
    // Client: reset the send-dedup so a reconnect re-streams, and drop any pending apply blob +
    // its assembler so a rejoin waits for a FRESH host push (never applies a stale inventory to
    // a new world).
    g_lastSentHash = 0;
    g_lastPoll = Clock::time_point{};
    g_hasPendingApply.store(false, std::memory_order_release);
    g_pendingApply = ue_wrap::inventory::PlayerInventory{};
    g_clientAssembler.Clear();
}

void FlushAllToDisk() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) FlushSlot(slot);
}

void EnsurePlayerFile(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // the host owns the per-player files
    const std::string& guid = coop::player_handshake::GuidForSlot(peerSlot);
    if (guid.empty()) {
        UE_LOGI("player_inventory: slot %d has no GUID yet (Join not landed / pre-v73 peer) -- "
                "no file this edge", peerSlot);
        return;
    }
    const fs::path file = PlayerFilePath(guid);
    if (file.empty()) {
        UE_LOGW("player_inventory: cannot build path for slot %d guid=%s "
                "(SaveGamesDir / HostSlot empty) -- skipping", peerSlot, guid.c_str());
        return;
    }
    std::error_code ec;
    if (fs::exists(file, ec)) {
        UE_LOGI("player_inventory: slot %d guid=%s -- file already present ('%ls')",
                peerSlot, guid.c_str(), file.c_str());
        return;
    }
    // First join for this GUID on this save -> write an EMPTY-inventory file in the real
    // magic+FNV format (NOT a raw placeholder), so the on-disk format is uniform. The client's
    // inventory stream overwrites it ~1 s later if it has items.
    const std::vector<uint8_t> empty = coop::inventory_wire::Serialize(ue_wrap::inventory::PlayerInventory{});
    const std::string nick = NarrowNick(coop::player_handshake::NicknameForSlot(peerSlot));
    if (WriteBlobFile(file, empty, nick))
        UE_LOGI("player_inventory: created empty inventory file for slot %d guid=%s ('%ls')",
                peerSlot, guid.c_str(), file.c_str());
}

}  // namespace coop::player_inventory_sync
