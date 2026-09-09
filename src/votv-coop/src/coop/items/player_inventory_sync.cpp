// coop/items/player_inventory_sync.cpp -- see coop/items/player_inventory_sync.h.

#include "coop/items/player_inventory_sync.h"

#include "coop/net/blob_chunks.h"
#include "ue_wrap/core/paths.h"
#include "coop/config/config.h"
#include "coop/items/inventory_wire.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"
#include "coop/save/save_transfer.h"
#include "ue_wrap/actors/begin_equipment.h"  // GiveFromClass, the starter-kit probe
#include "ue_wrap/engine/engine.h"      // SetSaveObjectReadyHook, the pre-materialise apply point
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
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

// Host: the per-slot received-blob state and the reassembler.
coop::blob_chunks::Assembler g_assembler;
struct HostEntry {
    std::string          guid;
    std::string          nick;                // a snapshot at receive (game thread): the shutdown flush runs off the game thread
    std::vector<uint8_t> blob;
    uint64_t             hash = 0;
    bool                 dirty = false;       // received, not yet flushed to disk
    Clock::time_point    lastWrite{};         // 15 s rate-limit
};
std::array<HostEntry, coop::net::kMaxPeers> g_hostBySlot;

// Client: the send-dedup state.
uint64_t          g_lastSentHash = 0;
uint32_t          g_sendSeq = 0;
Clock::time_point g_lastPoll{};
Clock::time_point g_lastSweep{};

constexpr auto kClientPoll  = std::chrono::seconds(1);
constexpr auto kWriteRate   = std::chrono::seconds(15);
constexpr auto kAsmTtl      = std::chrono::seconds(30);

// The live per-player apply on join. The host pushes each joiner its persisted inventory
// (coop_players/<slot>/<guid>.json); the client buffers it and the pre-materialise save-object
// hook substitutes it into the freshly loaded save object before the native load builds the
// live world from it, so the joiner gets their own items, not the host's save inventory. The
// only fallback is the hook's no-op for a blob that never arrived, which keeps the loaded
// inventory rather than wiping.

// Client: the host's on-join apply blob, reassembled here (a separate assembler from the
// host's receive path, since host-to-client is a distinct stream direction).
coop::blob_chunks::Assembler g_clientAssembler;
ue_wrap::inventory::PlayerInventory g_pendingApply;
std::atomic<bool> g_hasPendingApply{false};

// Host: the per-slot send sequence for the on-join push (independent of the client stream's
// sequence space; assembler keys are per sender and sequence, so they never collide).
std::array<uint32_t, coop::net::kMaxPeers> g_hostSendSeq{};
// Host: the per-slot pushed-this-connection latch. The push must land in the joiner's
// pre-world window (the connect-replay edge fires at world-ready, too late), so it is driven
// from the host tick the instant the slot is connected and its join GUID has arrived, well
// before the client finishes its save transfer and load. Reset on disconnect.
std::array<bool, coop::net::kMaxPeers> g_applySentToSlot{};

// Build <gameDir>/coop_players/<hostSlot>/<guid>.json, empty if any piece is missing. Stored
// in the game folder beside the ini and the log rather than in AppData, so the per-player
// files are easy to find and hand-edit; keyed per host save slot, so different worlds keep
// separate inventories.
fs::path PlayerFilePath(const std::string& guid) {
    // Defence in depth (the wire boundary already validates): a GUID that is not exactly 32 hex
    // characters never becomes a path component; an empty path makes every write no-op, so a
    // non-hex GUID cannot traverse.
    if (!coop::player_handshake::IsValidGuid(guid)) return {};
    const std::wstring base = ue_wrap::paths::ExeDir();
    if (base.empty()) return {};
    const std::wstring slot = coop::save_transfer::HostSlot();
    if (slot.empty()) return {};
    return fs::path(base) / L"coop_players" / slot / (std::wstring(guid.begin(), guid.end()) + L".json");
}

std::string Hex(const std::vector<uint8_t>& b) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s.push_back(k[c >> 4]); s.push_back(k[c & 0xF]); }
    return s;
}

// The nick goes into a JSON string field below. Encoding is the codec's job; escaping is this
// site's, because the container is JSON. Raw UTF-8 is valid inside a JSON string, only the
// two structural metacharacters have to go, and they cannot appear inside a multi-byte
// sequence (continuation bytes are all above 0x7F), so dropping them cannot corrupt one.
std::string NickForJson(const std::wstring& w) {
    std::string s = coop::text::CapUtf8Bytes(coop::text::ToUtf8(w), coop::text::kNickMaxBytes);
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == '"' || c == '\\'; }),
            s.end());
    return s;
}

// Persist `blob` to `file`: a magic, an FNV integrity hash and a readable nick and last-seen,
// written atomically (a temp file and a rename) and keeping a .bak of the last good file so a
// corrupt hand edit can be recovered. False on I/O failure, logged.
bool WriteBlobFile(const fs::path& file, const std::vector<uint8_t>& blob, const std::string& nick) {
    if (file.empty()) return false;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        UE_LOGW("player_inventory: create_directories('%ls') failed: %s",
                file.parent_path().c_str(), ec.message().c_str());
        return false;
    }
    // Keep the last good file as .bak before overwriting.
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

// Decode a hex string of either case to bytes. False on an odd length or a non-hex digit.
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

// Extract the value of a string field from our flat JSON (no nesting or escapes are used).
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

// Parse one persisted inventory file into `outBlob`. Defensive against a hand edit: requires
// the magic, a parseable hex blob and a matching FNV. False on any failure (the caller tries
// the .bak, then empty); never throws, never returns unverified bytes.
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

// Read a peer GUID's persisted inventory blob, FNV-verified, with the .bak fallback. False
// when neither file is valid, and the caller uses an empty inventory: a corrupt or missing
// file can never leak another player's inventory or crash; the player starts empty.
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

// Flush one host slot's pending blob to its file, ignoring the rate limit (disconnect and
// shutdown). Clears dirty.
void FlushSlot(int slot) {
    if (slot < 0 || slot >= coop::net::kMaxPeers) return;
    HostEntry& e = g_hostBySlot[slot];
    if (!e.dirty || e.guid.empty()) return;
    // The nick snapshot captured at receive, on the game thread: this is reachable from the
    // shutdown flush off the game thread, so it must touch no game-thread-only state (the
    // nickname table is one).
    if (WriteBlobFile(PlayerFilePath(e.guid), e.blob, e.nick)) {
        e.dirty = false;
        e.lastWrite = Clock::now();
        UE_LOGI("player_inventory: flushed slot %d guid=%s (%zu-byte blob) to disk",
                slot, e.guid.c_str(), e.blob.size());
    }
}

// Client: poll the live inventory at 1 Hz, serialise, and stream it to the host on change.
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

// Host: sweep stale half-assemblies, flush the rate-limited dirty blobs, and push each newly
// connected joiner its per-player apply blob once (the pre-world connect edge).
void HostPersistTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastSweep < std::chrono::seconds(1)) return;
    g_lastSweep = now;
    g_assembler.Sweep(now, kAsmTtl);
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        HostEntry& e = g_hostBySlot[slot];
        if (e.dirty && now - e.lastWrite >= kWriteRate) FlushSlot(slot);
        // The connect-edge push: once a slot is connected and its GUID has arrived (carried in the
        // join), send it its persisted inventory once, latching on a successful enqueue only, so a
        // channel-busy refusal or a not-yet-arrived GUID retries on the next 1 Hz tick. Reliable
        // delivery carries the single blob to the joiner during its pre-world wait, where the
        // receiver, installed before any world, buffers it for the save-object hook.
        if (!g_applySentToSlot[slot] && s->IsSlotConnected(slot) &&
            !coop::player_handshake::GuidForSlot(slot).empty()) {
            if (SendInventoryToSlot(slot)) g_applySentToSlot[slot] = true;
        }
    }
}

// Client: the engine's pre-materialise hook, registered in Install. Fires on the game thread
// with the freshly loaded save object, before the native load builds the world from it, the
// one window to substitute this client's inventory. Self-gates: only a client, only with a
// pending blob (the join boot waits for it). On a miss it leaves the loaded inventory rather
// than wiping; it never destroys data it cannot replace.
void OnSaveObjectReady(void* saveSlotObject) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;  // only a joining client applies
    if (!g_hasPendingApply.load(std::memory_order_acquire)) {
        UE_LOGW("player_inventory: SaveObjectReady but no apply blob arrived yet -- leaving the "
                "loaded inventory (NOT wiping). The join boot should have waited for it.");
        return;
    }
    if (ue_wrap::inventory::ApplyToSaveObject(saveSlotObject, g_pendingApply)) {
        UE_LOGI("player_inventory[client]: applied per-player inventory to save object %p "
                "(inventory=%zu equip=%zu hold=%zu) -- RETIRES the host-save inheritance",
                saveSlotObject, g_pendingApply.inventory.size(),
                g_pendingApply.equipment.size(), g_pendingApply.hold.size());
    } else {
        UE_LOGE("player_inventory[client]: ApplyToSaveObject FAILED on %p -- the client will "
                "fall back to the inventory its own save loaded (no wipe)", saveSlotObject);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Arm the engine's pre-materialise apply point (a no-op until a client join has a pending
    // blob). Re-arming across stop and start is harmless.
    ue_wrap::engine::SetSaveObjectReadyHook(&OnSaveObjectReady);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected()) {
        if (s->role() == coop::net::Role::Client) ClientStreamTick(s);
        else                                      HostPersistTick(s);
    }

    // The read-verify selftest (ini inventory_selftest=1). One-shot; a dev diagnostic.
    static const bool s_selftest = ::coop::config::ResolveFlag(::coop::config_registry::rows::inventory_selftest);
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
    {   // DIAG: name what the LOCAL player actually has post-spawn (find flashlight/glasses/compass).
        auto narrow = [](const std::wstring& w) { return std::string(w.begin(), w.end()); };
        for (size_t i = 0; i < inv.inventory.size(); ++i)
            UE_LOGI("  selftest inv[%zu]: className='%s' key='%s'", i,
                    narrow(inv.inventory[i].className).c_str(), narrow(inv.inventory[i].key).c_str());
        for (size_t i = 0; i < inv.equipment.size(); ++i)
            UE_LOGI("  selftest eq[%zu]: propName='%s' className='%s'", i,
                    narrow(inv.equipment[i].propName).c_str(), narrow(inv.equipment[i].data.className).c_str());
        for (size_t i = 0; i < inv.hold.size(); ++i)
            UE_LOGI("  selftest hold[%zu]: propName='%s' className='%s'", i,
                    narrow(inv.hold[i].propName).c_str(), narrow(inv.hold[i].data.className).c_str());
    }
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

    // A dev probe (ini starterkit_test=1): equip the three starters through the game's own
    // add-equipment path and re-read, confirming the canonical equip path adds them. One-shot,
    // riding the selftest.
    if (::coop::config::ResolveFlag(::coop::config_registry::rows::starterkit_test)) {
        for (const wchar_t* c : {L"prop_equipment_flashlight_C", L"prop_equipment_glasses_C",
                                 L"prop_equipment_compass_C"})
            ue_wrap::begin_equipment::GiveFromClass(c);
        ue_wrap::inventory::PlayerInventory after;
        if (ue_wrap::inventory::ReadAll(after))
            UE_LOGI("starterkit[probe]: after AddEquipment x3 -- inventory=%zu equip=%zu hold=%zu "
                    "(was %zu/%zu/%zu)", after.inventory.size(), after.equipment.size(),
                    after.hold.size(), inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    }
}

void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;

    // The client direction: the host pushed this client its per-player apply blob.
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

    // The host direction: a client slot streamed its live inventory to persist.
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
    e.nick = NickForJson(coop::player_handshake::NicknameForSlot(senderPeerSlot));  // GT snapshot
    e.dirty = true;
    // Write now if the rate-limit window has passed; else the host tick flushes it.
    if (Clock::now() - e.lastWrite >= kWriteRate) FlushSlot(senderPeerSlot);
}

// The single-player starter items: the game's begin-equipment logic gives exactly these three
// at New Game, and the host save retains them across play.
int StarterIndex(const std::wstring& className) {
    if (className == L"prop_equipment_flashlight_C") return 0;
    if (className == L"prop_equipment_glasses_C")    return 1;
    if (className == L"prop_equipment_compass_C")    return 2;
    return -1;
}

// Build the first-join starter kit (flashlight, glasses, compass) by reading the host's own
// live inventory and keeping only those three, one per class: valid, complete save records
// with no hardcoded struct layout and none of the host's other items. The worn-equipment
// array shape is preserved (non-starter and duplicate slots cleared) so the kit keeps the
// New Game layout. False if no starter item is present (the host dropped them, or the save
// slot is unresolvable), and the caller sends empty. Game thread. The copied records carry
// the host's item keys, benign while the items are held or worn inventory data rather than
// world actors; only a simultaneous drop of the same starter item by several peers would
// collide on the world-prop key.
bool BuildFirstJoinStarterKit(ue_wrap::inventory::PlayerInventory& out) {
    ue_wrap::inventory::PlayerInventory host;
    if (!ue_wrap::inventory::ReadAll(host)) return false;
    bool got[3] = {false, false, false};
    for (const auto& r : host.inventory) {
        const int k = StarterIndex(r.className);
        if (k >= 0 && !got[k]) { got[k] = true; out.inventory.push_back(r); }
    }
    out.equipment = host.equipment;  // keep the worn-slot array shape
    for (auto& e : out.equipment) {
        const int k = StarterIndex(e.data.className);
        if (k < 0 || got[k]) e = ue_wrap::inventory::EquipRecord{};  // clear non-starter / dup slot
        else got[k] = true;
    }
    out.hold = host.hold;
    for (auto& e : out.hold) {
        const int k = StarterIndex(e.data.className);
        if (k < 0 || got[k]) e = ue_wrap::inventory::EquipRecord{};
        else got[k] = true;
    }
    return got[0] || got[1] || got[2];
}

bool SendInventoryToSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return false;  // host pushes; clients receive
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const std::string& guid = coop::player_handshake::GuidForSlot(peerSlot);
    if (guid.empty()) {
        UE_LOGI("player_inventory: slot %d has no GUID yet -- not sending an apply blob this edge",
                peerSlot);
        return false;  // not sent -> caller does not latch; retries when the GUID lands
    }
    std::vector<uint8_t> blob;
    if (!ReadBlobFile(guid, blob)) {
        // First join (no persisted file, or corrupt with no .bak): seed the starter kit so the
        // player is not dropped into the host's world empty-handed (a loaded coop save runs no
        // begin-equipment, so they would be). Read off the host's own live inventory, filtered to
        // the three starter classes; if the kit is absent, degrade to empty, never to the host's
        // other items or another player's.
        ue_wrap::inventory::PlayerInventory kit;
        if (BuildFirstJoinStarterKit(kit)) {
            blob = coop::inventory_wire::Serialize(kit);
            UE_LOGI("player_inventory: slot %d guid=%s -- first join, seeding SP starter kit "
                    "(inventory=%zu equip-slots=%zu)", peerSlot, guid.c_str(),
                    kit.inventory.size(), kit.equipment.size());
        } else {
            blob = coop::inventory_wire::Serialize(ue_wrap::inventory::PlayerInventory{});
            UE_LOGI("player_inventory: slot %d guid=%s -- first join, starter kit unavailable; sending EMPTY",
                    peerSlot, guid.c_str());
        }
    }
    if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::PlayerInventoryBlob,
                                          ++g_hostSendSeq[peerSlot], blob)) {
        UE_LOGI("player_inventory: sent slot %d guid=%s its per-player inventory (%zu-byte blob)",
                peerSlot, guid.c_str(), blob.size());
        return true;
    }
    UE_LOGW("player_inventory: send to slot %d refused (channel busy) -- will RETRY next tick "
            "(NOT latched as sent)", peerSlot);
    return false;
}

bool HasPendingApply() { return g_hasPendingApply.load(std::memory_order_acquire); }

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
    // Client: reset the send dedup so a reconnect re-streams, and drop any pending apply blob and
    // its assembler, so a rejoin waits for a fresh host push and never applies a stale inventory
    // to a new world.
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
        UE_LOGI("player_inventory: slot %d has no GUID yet (Join not landed) -- "
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
    // First join for this GUID on this save: write an empty-inventory file in the real magic and
    // FNV format, so the on-disk format is uniform. The client's inventory stream overwrites it a
    // second later if it has items.
    const std::vector<uint8_t> empty = coop::inventory_wire::Serialize(ue_wrap::inventory::PlayerInventory{});
    const std::string nick = NickForJson(coop::player_handshake::NicknameForSlot(peerSlot));
    if (WriteBlobFile(file, empty, nick))
        UE_LOGI("player_inventory: created empty inventory file for slot %d guid=%s ('%ls')",
                peerSlot, guid.c_str(), file.c_str());
}

}  // namespace coop::player_inventory_sync
