// coop/player_inventory_sync.cpp -- see coop/player_inventory_sync.h.

#include "coop/items/player_inventory_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/config/config.h"
#include "coop/items/inventory_wire.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"
#include "coop/save/save_transfer.h"
#include "coop/config/config.h"     // ModuleDir -- the coop_players store now lives in the GAME folder
#include "ue_wrap/actors/begin_equipment.h"  // RULE-1 first-join: the game's own getData->AddEquipment equip
#include "ue_wrap/engine/engine.h"      // Inc 4: SetSaveObjectReadyHook -- the pre-materialize apply point
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/log.h"

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

// ---- The live per-player apply on join (host->client push + the SaveObjectReadyHook) ----
// The host pushes each joiner its persisted inventory (coop_players/<slot>/<guid>.json); the client
// buffers it (g_pendingApply) and the pre-materialize SaveObjectReadyHook substitutes it into the
// freshly-loaded save object BEFORE the native loadObjects() builds the live world from it -- so the
// joiner gets THEIR OWN per-player items, not the host's save inventory. This IS the inventory
// behaviour now (no rollout flag -- RULE 2: a flag that kept the old v56 host-save inheritance as a
// parallel path was migration baggage, retired once the apply was verified end-to-end). The only
// fallback is the SaveObjectReadyHook no-op below, for the degenerate case where the apply blob
// genuinely never arrived -- it KEEPS the loaded inventory rather than wiping.

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

// Build <gameDir>/coop_players/<hostSlot>/<guid>.json. Empty path if any piece is missing.
// User request (2026-06-15): store in the GAME folder (next to the payload DLL / multivoid.ini/.log), NOT in
// AppData -- so the per-player inventory jsons are easy to find + hand-edit. Still keyed per host
// SAVE SLOT (a subfolder under coop_players) so different worlds keep separate inventories.
fs::path PlayerFilePath(const std::string& guid) {
    // Defense in depth (the wire boundary already validates): a GUID that is not exactly 32 hex
    // chars must NEVER become a path component -- return empty so every write path no-ops cleanly,
    // foreclosing path traversal even if a non-hex guid ever reaches here. (Adversarial-verify HIGH.)
    if (!coop::player_handshake::IsValidGuid(guid)) return {};
    const std::wstring base = coop::config::ModuleDir();
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
        // Connect-edge apply push: once a slot is connected AND its GUID has arrived (carried in the
        // Join), send it its persisted per-player inventory ONCE, latching on a successful enqueue.
        // SendInventoryToSlot returns false on a channel-busy refusal (or a not-yet-arrived GUID), so
        // an un-latched failure simply retries on the next 1 Hz tick until it goes out -- the original
        // "latch even on refusal" bug ("no inventory blob") is fixed by latching on TRUE only. GNS
        // reliable delivery guarantees the single enqueued blob reaches the joiner during its pre-world
        // wait, where the (now PRE-WORLD-installed) receiver buffers it for OnSaveObjectReady. No spray
        // is needed: the earlier "never arrived" symptom was the receiver's g_session being null during
        // the wait (the world-gated Install bug, now fixed at StartCoopSession), not lossy delivery.
        if (!g_applySentToSlot[slot] && s->IsSlotConnected(slot) &&
            !coop::player_handshake::GuidForSlot(slot).empty()) {
            if (SendInventoryToSlot(slot)) g_applySentToSlot[slot] = true;
        }
    }
}

// CLIENT: the engine's pre-materialize hook (registered in Install). Fires on the game thread
// with the freshly loaded/created save object, BEFORE the native loadObjects() builds the world
// from it -- the one window to substitute this client's per-player inventory. Self-gates: only a
// CLIENT, only with a pending blob (the join boot waits for it). On a miss it SKIPS (leaves the
// loaded inventory) rather than wiping -- never destroys data it can't replace.
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
    static const bool s_selftest = ::coop::config::IsIniKeyTrue("inventory_selftest");
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

    // RULE-1 proper-fix PROBE (ini starterkit_test=1, dev-only): equip the 3 SP starters via the
    // game's OWN AddEquipment (begin_equipment::GiveFromClass) + re-read, confirming the canonical
    // equip path adds the items BEFORE it's wired to the first-join edge. One-shot (rides this
    // one-shot selftest). Removed once first-join uses it.
    if (::coop::config::IsIniKeyTrue("starterkit_test")) {
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

// The SP New-Game starter items (RE 2026-06-16: a New Game gives the player exactly these three --
// glasses rides the MAIN inventory, flashlight + compass ride the worn-equipment slot array; the
// game's `beginEquipment` logic adds them at New Game and the host save retains them across play).
int StarterIndex(const std::wstring& className) {
    if (className == L"prop_equipment_flashlight_C") return 0;
    if (className == L"prop_equipment_glasses_C")    return 1;
    if (className == L"prop_equipment_compass_C")    return 2;
    return -1;
}

// Build the first-join STARTER KIT (flashlight + glasses + compass) by reading the host's OWN live
// inventory and keeping only those three (one per class) -- valid, complete save records with no
// hardcoded struct layout and no inheriting the host's other items. The 8-slot worn-equipment array
// shape is preserved (non-starter / duplicate slots cleared) so the kit keeps the New-Game layout.
// Returns false if no starter item is present (host dropped them / saveSlot unresolvable) -> caller
// sends EMPTY. Game thread (ReadAll).
// NOTE: the copied records carry the host's item KEYS -- benign while the items are held/worn
// inventory DATA (not world actors); only a simultaneous DROP of the same starter item by multiple
// peers would collide on the world-prop key. Follow-up (regenerate keys) only if that's observed.
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
        // First join (no persisted file / corrupt + no .bak): seed the SP starter kit (flashlight +
        // glasses + compass) so the player isn't dropped into the host's world empty-handed (it is
        // confirmed empty otherwise -- a loaded coop save runs no begin-equipment). Read off the
        // host's own live inventory, filtered to the 3 starter classes. On failure / kit absent,
        // degrade to EMPTY (fail-safe -- never inherit the host's other or another player's items).
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
