// coop/save/save_transfer.cpp -- the host's world to a joining client: a live capture of the host's
// world into a scratch slot (or the on-disk slot behind a torn-read guard), chunked over the
// bulk lane behind a Begin, CRC-checked and written to the client's coop slot. The photograph
// only; what the world did while it travelled is coop/save/join_window_baseline, captured in the
// same breath here and flushed from this tick. See coop/save/save_transfer.h.

#include "coop/save/save_transfer.h"

#include "coop/comms/chat_feed.h"  // the join-window cue, behind the pile_delta_probe flag
#include "coop/element/element.h"   // Element::GetActor
#include "coop/element/registry.h"  // the host's eid-to-actor lookup
#include "coop/config/config.h"  // the test-cue flag
#include "coop/config/config_registry.h"  // [dev] stall_world_stream_at_pct, the stalled-download drill
#include "coop/interactables/meadow_db_sync.h"  // the join-seed multiset snapshot at the blob instant
#include "coop/interactables/signal_sync.h"  // the seed snapshot capture and cancel
#include "coop/world/email_sync.h"           // the seed snapshot capture and cancel
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"  // the blob-versus-live key diff
#include "coop/props/save_identity_bind.h"  // the client's eid-range bind
#include "coop/props/save_identity_map.h"  // the host's keyless index-to-eid map
#include "coop/session/join_beacon.h"  // the joiner hears which phase this stream is in
#include "coop/save/join_window_baseline.h"  // the capture instant's baselines and their flush
#include "coop/save/save_guard.h"
#include "coop/save/save_indicator_suppress.h"  // detect the SAVED HUD across the join scratch save
#include "ue_wrap/engine/engine.h"      // the host's current prop position
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"        // IsChipPile: a grabbed clump belongs to the convert stream
#include "ue_wrap/core/reflection.h"  // IsLive
#include "ue_wrap/engine/save_capture.h"
#include "ue_wrap/world/game_mode.h"   // the host's own mode, for the begin

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>     // the drift log
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace coop::save_transfer {

namespace {

coop::net::Session* g_session = nullptr;  // set once at Install (boot), read thereafter

// CRC-32 (IEEE, table-driven); both sides of the wire use it.
uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static std::atomic<bool> init{false};
    if (!init.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init.store(true, std::memory_order_release);
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool ReadWholeFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.good() || f.eof();
}

// The host side, game thread only: OnRequest, TickHost, CancelForSlot.

std::wstring g_hostSlot;  // the slot the host's world was loaded from
std::atomic<uint32_t> g_hostSlotSerial{0};  // one per SetHostSlot, i.e. per world loaded to host (the boot load names it off the game thread)

// The torn-read guard: VOTV's saveToSlot writes the .sav in place, so a join landing mid-write
// would read a torn blob. The file is trusted only when its size and mtime are stable across two
// polls this far apart and two consecutive full reads are CRC-identical.
constexpr uint64_t kStablePollMs = 300;

struct HostStream {
    bool     active = false;
    bool     blobReady = false;
    // The pump owns Begin's delivery: a fire-and-forget send under backpressure was silently
    // deleted and the chunks followed with no announce. Staged here, TickHost try-sends it
    // (retrying next tick) before any chunk, so Begin always precedes its chunks in the lane.
    bool     beginSent = false;
    coop::net::SaveTransferBeginPayload beginPayload{};
    uint32_t nextChunk = 0;
    uint32_t chunkCount = 0;
    // The stable-read probe state.
    uint64_t  lastSize = 0;
    int64_t   lastMtime = 0;
    uint64_t  lastProbeTick = 0;   // GetTickCount64 of the last probe
    int       stableCount = 0;
    uint32_t  firstReadCrc = 0;
    bool      haveFirstRead = false;
    int       readAttempts = 0;
    bool      stallSaid = false;   // [dev] stall_world_stream_at_pct said its piece once
    std::vector<uint8_t> blob;     // captured stable blob (per-slot copy; 17MB,
                                   // freed on completion -- joins are rare)
};
HostStream g_host[coop::net::kMaxPeers];
// Per slot, whether its world was taken for this connection's join (WorldTakenFor).
bool g_worldTaken[coop::net::kMaxPeers] = {};

// Chunks per TickHost pass per slot: a ~13 MB/s ceiling at 60 Hz; the send buffer's backpressure
// (a failed send stops the pass) is the real pacer on a slower link.
constexpr int kChunksPerTick = 4;

// The mode byte the host announces. The header has no "unknown" encoding, so an unreadable mode
// can only be sent as story -- but the GameInstance is immortal and the property is resolved by
// name, so on a host serving a world the read is a substrate fault, not a state, and says so.
uint8_t HostModeByte_(int slot) {
    const int mode = ue_wrap::game_mode::ReadLocal();
    if (!ue_wrap::game_mode::IsValid(mode)) {
        UE_LOGE("save_transfer: slot %d -- the host's own game mode did not read (%d); "
                "the joiner is told story", slot, mode);
        return static_cast<uint8_t>(ue_wrap::game_mode::kStory);
    }
    UE_LOGI("save_transfer: slot %d -- the joiner loads in the host's mode %d (%s)", slot, mode,
            ue_wrap::game_mode::NameOrOrdinal(mode).c_str());
    return static_cast<uint8_t>(mode);
}

// A "no save" announce (zero bytes, zero chunks) as a pump stream, so TickHost delivers it with
// retry; a backpressure-deleted one left the client waiting forever. The mode rides it too: the
// guest fresh-boots, and a fresh world of the host's mode is still the host's world.
void ArmBeginNoSave_(int slot) {
    HostStream& hs = g_host[slot];
    hs = HostStream{};
    hs.active = true;
    hs.blobReady = true;
    hs.beginSent = false;
    hs.beginPayload = coop::net::SaveTransferBeginPayload{};
    hs.beginPayload.totalBytes = 0;
    hs.beginPayload.gameMode = HostModeByte_(slot);
    hs.chunkCount = 0;
    UE_LOGW("save_transfer: no readable host save for slot %d -- client will fresh-boot", slot);
}

// A captured blob into the host stream, with its Begin staged; shared by the live-capture path
// and the torn-read fallback. `crc` is the caller's.
void BeginStreamFromBlob_(int slot, HostStream& hs, std::vector<uint8_t>&& bytes, uint32_t crc,
                          uint32_t sidecarBytes = 0) {
    hs.active = true;
    hs.blob = std::move(bytes);  // already framed (identity sidecar prepended) by the caller
    hs.blobReady = true;
    g_worldTaken[slot] = true;
    hs.nextChunk = 0;
    hs.chunkCount = static_cast<uint32_t>(
        (hs.blob.size() + coop::net::kSaveChunkBytes - 1) / coop::net::kSaveChunkBytes);

    // The announce is staged; TickHost sends it, with retry, before any chunk.
    hs.beginSent = false;
    hs.beginPayload = coop::net::SaveTransferBeginPayload{};
    hs.beginPayload.totalBytes = static_cast<uint32_t>(hs.blob.size());
    hs.beginPayload.chunkCount = hs.chunkCount;
    hs.beginPayload.crc32 = crc;
    // The host's own mode, which the joiner's load forces: the slot's zcoop_ prefix names no mode.
    hs.beginPayload.gameMode = HostModeByte_(slot);
    hs.beginPayload.sidecarBytes = sidecarBytes;  // the framed identity map leads the stream
}

// One stable-read attempt for a slot still capturing; true once the blob is captured.
bool TryCaptureBlob_(int slot, HostStream& hs) {
    const uint64_t now = ::GetTickCount64();
    if (now - hs.lastProbeTick < kStablePollMs) return false;  // wait out the poll gap
    hs.lastProbeTick = now;

    const fs::path file = coop::save_guard::SaveGamesDir() / (g_hostSlot + L".sav");
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    if (ec) {
        if (++hs.readAttempts >= 4) { ArmBeginNoSave_(slot); }  // R-4b: retried by the pump
        return false;
    }
    const auto mtime = fs::last_write_time(file, ec).time_since_epoch().count();
    if (ec) return false;

    if (size != hs.lastSize || mtime != hs.lastMtime) {
        // Changed since the last probe (the game may be mid-save): the stability count restarts,
        // which also covers the first probe.
        hs.lastSize = size;
        hs.lastMtime = mtime;
        hs.stableCount = 1;
        hs.haveFirstRead = false;
        return false;
    }
    if (++hs.stableCount < 3) return false;  // need 2 stable gaps (3 identical probes)

    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(file, bytes) || bytes.empty()) {
        if (++hs.readAttempts >= 4) { ArmBeginNoSave_(slot); }  // R-4b: retried by the pump
        return false;
    }
    const uint32_t crc = Crc32(bytes.data(), bytes.size());
    if (!hs.haveFirstRead) {
        // The first full read keeps its CRC and the next read must match: the double read closes
        // the in-place-write window.
        hs.haveFirstRead = true;
        hs.firstReadCrc = crc;
        return false;
    }
    if (crc != hs.firstReadCrc) {
        UE_LOGW("save_transfer: slot %d double-read CRC mismatch (file changing) -- re-probing", slot);
        hs.haveFirstRead = false;
        hs.stableCount = 0;
        return false;
    }

    BeginStreamFromBlob_(slot, hs, std::move(bytes), crc);
    UE_LOGI("save_transfer: slot %d streaming CANONICAL slot '%ls' (stale fallback; %u bytes, "
            "%u chunks, crc=0x%08X)",
            slot, g_hostSlot.c_str(), static_cast<uint32_t>(hs.blob.size()), hs.chunkCount, crc);
    return true;
}

// The client side: the net-thread sinks, and the state the game thread polls.

std::mutex g_cliMu;
ClientState g_cliState = ClientState::Idle;
bool     g_cliArmed = false;
bool     g_cliRequested = false;
uint32_t g_cliTotal = 0;
uint32_t g_cliChunkCount = 0;
uint32_t g_cliCrc = 0;
uint8_t  g_cliGameMode = 0;
uint32_t g_cliSidecarBytes = 0;  // leading bytes of the buffer that are the framed identity map
bool     g_cliHaveBegin = false;
uint32_t g_cliChunksSeen = 0;
std::vector<uint8_t> g_cliBuf;

std::wstring CoopSlotFileNameNoExt_() {
    wchar_t buf[32];
    swprintf(buf, 32, L"zcoop_%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
    return buf;
}

// Both inputs (the Begin and the bytes, each through its net-thread sink) funnel here under the
// mutex: when the blob is whole it is CRC-verified and written to the slot.
void MaybeFinishLocked_() {
    if (!g_cliHaveBegin || g_cliState != ClientState::Receiving) return;
    if (g_cliBuf.size() < g_cliTotal) return;
    if (g_cliBuf.size() > g_cliTotal) {
        UE_LOGE("save_transfer: received %zu bytes > announced %u -- failing",
                g_cliBuf.size(), g_cliTotal);
        g_cliState = ClientState::Failed;
        return;
    }
    const uint32_t crc = Crc32(g_cliBuf.data(), g_cliBuf.size());  // over the WHOLE framed stream (sidecar+blob)
    if (crc != g_cliCrc) {
        UE_LOGE("save_transfer: blob CRC mismatch (got 0x%08X want 0x%08X) -- failing",
                crc, g_cliCrc);
        g_cliState = ClientState::Failed;
        return;
    }
    // The host may have prepended the framed index-to-eid identity sidecar: framing, not part of
    // the .sav, so it is stripped and the game's loadObjects sees a clean GVAS blob. The split
    // length comes from the CRC-verified Begin, so it holds even if the map's inner framing is
    // malformed.
    const uint8_t* blobStart = g_cliBuf.data();
    size_t blobLen = g_cliBuf.size();
    if (g_cliSidecarBytes > 0) {
        if (g_cliSidecarBytes > g_cliBuf.size()) {
            UE_LOGE("save_transfer: sidecarBytes=%u exceeds blob %zu -- failing",
                    g_cliSidecarBytes, g_cliBuf.size());
            g_cliState = ClientState::Failed;
            return;
        }
        coop::save_identity_map::IdMap rxMap;
        size_t consumed = 0;
        if (coop::save_identity_map::DeserializeSidecar(g_cliBuf.data(), g_cliSidecarBytes, rxMap, consumed) &&
            consumed == g_cliSidecarBytes) {
            coop::save_identity_map::LogReceivedMap(rxMap);
            // The map goes to the eid-range bind before the harness loads the slot (the natives
            // spawn during that loadObjects); a no-op unless the dev flag is on.
            coop::save_identity_bind::SetReceivedMap(rxMap);
        } else {
            UE_LOGE("save_transfer: identity sidecar parse failed (sidecarBytes=%u consumed=%zu) -- map "
                    "ignored (stripping the bytes anyway; the .sav blob follows)", g_cliSidecarBytes, consumed);
        }
        blobStart = g_cliBuf.data() + g_cliSidecarBytes;
        blobLen = g_cliBuf.size() - g_cliSidecarBytes;
    }
    const fs::path dir = coop::save_guard::SaveGamesDir();
    const fs::path tmp = dir / (CoopSlotFileNameNoExt_() + L".sav.part");
    const fs::path dst = dir / (CoopSlotFileNameNoExt_() + L".sav");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(blobStart),
                           static_cast<std::streamsize>(blobLen))) {
            UE_LOGE("save_transfer: slot write failed ('%ls')", tmp.c_str());
            g_cliState = ClientState::Failed;
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) {
        UE_LOGE("save_transfer: slot rename failed ('%ls' -> '%ls')", tmp.c_str(), dst.c_str());
        g_cliState = ClientState::Failed;
        return;
    }
    g_cliBuf.clear();
    g_cliBuf.shrink_to_fit();
    g_cliState = ClientState::ReadySlotWritten;
    UE_LOGI("save_transfer: host save written as '%ls' (%zu blob bytes [%u stream - %u sidecar], crc ok) "
            "-- ready to load", dst.c_str(), blobLen, g_cliTotal, g_cliSidecarBytes);
}

// Net thread, the session's bulk sink. A chunk is a u32 index plus raw bytes; one stream's chunks
// arrive in lane order through the single net thread, so a sequential index check suffices.
void BulkSink_(int senderPeerSlot, const uint8_t* data, int len) {
    if (senderPeerSlot != 0) return;  // host-originated only
    if (len < 4) return;
    // A chunk can never legitimately exceed the sender's own chunk size; the receive path bounds it
    // only by the u16 payload length otherwise.
    if (len > static_cast<int>(4 + coop::net::kSaveChunkBytes)) return;
    uint32_t idx = 0;
    std::memcpy(&idx, data, 4);
    std::lock_guard<std::mutex> lk(g_cliMu);
    if (g_cliState != ClientState::WaitingBegin && g_cliState != ClientState::Receiving) return;
    // Bytes with no announced size are refused. Safe because Begin is latched on this same thread:
    // the announce and the payload ride one in-order lane processed by the single net thread, and
    // the host queues Begin before the chunk pump runs, so Begin is always processed first. With
    // Begin drained on the game thread this branch was reachable by thread lag and the buffer had
    // to accept unannounced bytes, an unbounded growth; closing the window beats capping it.
    if (!g_cliHaveBegin) {
        UE_LOGE("save_transfer: chunk with no Begin (idx=%u, %d B) -- refusing (no announced size)",
                idx, len);
        g_cliState = ClientState::Failed;
        return;
    }
    if (idx != g_cliChunksSeen) {
        UE_LOGE("save_transfer: chunk out of order (idx=%u expected=%u) -- failing",
                idx, g_cliChunksSeen);
        g_cliState = ClientState::Failed;
        return;
    }
    ++g_cliChunksSeen;
    g_cliBuf.insert(g_cliBuf.end(), data + 4, data + len);
    MaybeFinishLocked_();
}

// Net thread, the session's SaveTransferBegin sink: the same thread as the bulk sink by design.
// The length is validated here, since the sink contract hands over raw bytes.
void BeginSink_(int senderPeerSlot, const uint8_t* data, int len) {
    if (senderPeerSlot != 0) return;  // host-originated only
    if (len < static_cast<int>(sizeof(coop::net::SaveTransferBeginPayload))) return;
    coop::net::SaveTransferBeginPayload p{};
    std::memcpy(&p, data, sizeof(p));
    OnBegin(p);
}

void DeleteFileLogged_(const fs::path& p) {
    std::error_code ec;
    if (fs::remove(p, ec)) UE_LOGI("save_transfer: deleted '%ls'", p.c_str());
}

}  // namespace

std::wstring CoopSlotName() { return CoopSlotFileNameNoExt_(); }

const std::wstring& HostSlot() { return g_hostSlot; }

uint32_t HostSlotSerial() { return g_hostSlotSerial.load(std::memory_order_acquire); }

void Install(coop::net::Session* session) {
    g_session = session;
    session->SetBulkSink(&BulkSink_);
    // The announce lands on the same thread as the payload it announces.
    session->SetSaveBeginSink(&BeginSink_);
}

// The host.

void SetHostSlot(const std::wstring& slot) {
    g_hostSlot = slot;
    g_hostSlotSerial.fetch_add(1, std::memory_order_release);
    UE_LOGI("save_transfer: host slot = '%ls'", slot.c_str());
}

// The throwaway slot the live host world is serialised into: the zcoop_ prefix means the boot
// sweep removes a crash leftover, and it is deleted the instant it is read. Never the host's
// canonical slot.
constexpr const wchar_t* kHostXferSlot = L"zcoop_hostxfer";

void OnRequest(int peerSlot) {
    if (!g_session || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    HostStream& hs = g_host[peerSlot];
    hs = HostStream{};  // reset any prior stream for this slot (rejoin)

    // The join-window cue, behind the pile_delta_probe flag: the joiner requested the save, so a
    // pile moved from here until the joiner's world-ready is in-window and reconciles by its
    // save-time key.
    static const bool s_pileProbe =
        coop::config::ResolveFlag(::coop::config_registry::rows::pile_delta_probe);
    if (s_pileProbe)
        coop::chat_feed::Push(L"[1c-test] JOIN-WINDOW OPEN -- joiner loading; move/drop test piles NOW (close at 'JOIN-WINDOW CLOSED')",
                              coop::chat_feed::Keep::Transient);

    // The host's world is serialised live, now, into a scratch slot instead of shipping the stale
    // on-disk save: an entity the host changed since its last autosave (a kerfur turned on, now an
    // NPC) is captured in its live state, so the joiner's loadObjects builds a correct world and
    // the reconcile has nothing to fix. The canonical slot is never named or written. On the game
    // thread (net_pump::Tick asserts it), so the save UFunctions are legal; the scratch file is
    // read synchronously, then deleted. The bracket around the capture DETECTS the native SAVED
    // HUD rather than suppressing it -- nothing suppresses it yet: the bracket spans the
    // synchronous capture, and a manual save uses a BP path the mod never calls, so a fire inside
    // it belongs to the join save.
    coop::save_indicator_suppress::Begin();
    const bool captured = ue_wrap::save_capture::CaptureLiveWorldToScratchSlot(kHostXferSlot);
    coop::save_indicator_suppress::End();
    if (captured) {
        const fs::path scratchFile =
            coop::save_guard::SaveGamesDir() / (std::wstring(kHostXferSlot) + L".sav");
        std::vector<uint8_t> bytes;
        bool got = ReadWholeFile(scratchFile, bytes) && !bytes.empty();
        // A second opinion, not the primary check, which is the producer's: save_capture refuses a
        // capture whose objectsData came back empty. This size ratio discriminates only a big
        // canonical against a small capture; a stale gamemode's full container serialises a
        // complete world that is not the host's, and nothing here sees that.
        if (got && !g_hostSlot.empty()) {
            std::error_code sizeEc;
            const uint64_t canonical =
                fs::file_size(coop::save_guard::SaveGamesDir() / (g_hostSlot + L".sav"), sizeEc);
            if (!sizeEc && canonical > 0 && bytes.size() * 8 < canonical) {
                UE_LOGW("save_transfer: slot %d -- live capture produced %zu B against a canonical "
                        "'%ls.sav' of %llu B: the game had not finished writing it. REFUSING to "
                        "stream a world we cannot vouch for; falling back to the canonical slot.",
                        peerSlot, bytes.size(), g_hostSlot.c_str(),
                        static_cast<unsigned long long>(canonical));
                got = false;
            }
        }
        DeleteFileLogged_(scratchFile);  // transient -- gone the instant it is read
        if (got) {
            // The identity sidecar, dev-gated: the index-to-eid map of the keyless save-loaded
            // natives, prepended to the blob so it travels inside the same CRC'd stream the client
            // loads and cannot desync from the blob it indexes. Without the flag the stream is
            // byte-identical to one with no sidecar. Built before the Begin, so the framing is part
            // of the CRC.
            std::vector<uint8_t> sidecar;  // empty unless the dev flag is on (then 12B header + 9B/entry)
            static const bool s_idMapLog =
                coop::config::ResolveFlag(::coop::config_registry::rows::save_identity_map_log);
            if (s_idMapLog) {
                coop::save_identity_map::IdMap idMap;
                coop::save_identity_map::BuildHostMap(idMap);              // logs its own per-family summary
                coop::save_identity_map::SerializeSidecar(idMap, sidecar);
                UE_LOGI("save_transfer: slot %d -- identity sidecar serialized: %zu entries -> %zu bytes "
                        "(prepended to the blob stream)", peerSlot, idMap.size(), sidecar.size());
            }
            const uint32_t sidecarBytes = static_cast<uint32_t>(sidecar.size());
            if (sidecarBytes) {
                std::vector<uint8_t> framed;
                framed.reserve(sidecar.size() + bytes.size());
                framed.insert(framed.end(), sidecar.begin(), sidecar.end());
                framed.insert(framed.end(), bytes.begin(), bytes.end());
                bytes.swap(framed);  // bytes := [sidecar][.sav blob]
            }
            const uint32_t crc = Crc32(bytes.data(), bytes.size());  // CRC over the FRAMED stream
            BeginStreamFromBlob_(peerSlot, hs, std::move(bytes), crc, sidecarBytes);
            UE_LOGI("save_transfer: slot %d streaming LIVE host world (%u bytes, %u chunks, "
                    "crc=0x%08X, sidecar=%u B)",
                    peerSlot, static_cast<uint32_t>(hs.blob.size()), hs.chunkCount, crc, sidecarBytes);
            // THE CAPTURE INSTANT. Every baseline the join reconcile reads is taken here, in the
            // same breath as the blob, by whoever owns it. Live-capture path only: the stale
            // fallback leaves each one empty and the divergence sweep keeps full responsibility
            // for that join.
            coop::join_window_baseline::CaptureForSlot(peerSlot);  // keyed keys + save-time xforms
            // The meadow-DB content-hash multiset; the ready-edge seed diffs it against the
            // then-live store. The stale fallback leaves it invalid, so no seed.
            coop::meadow_db_sync::CaptureJoinSnapshot(peerSlot);
            // The signal and email seeds capture at the same instant.
            coop::signal_sync::CaptureJoinSnapshot(peerSlot);
            coop::email_sync::CaptureJoinSnapshot(peerSlot);
            return;
        }
        // Only when the read itself failed: the plausibility gate above has already said why.
        if (bytes.empty())
        UE_LOGW("save_transfer: slot %d -- live scratch '%ls.sav' unreadable after capture; "
                "falling back to the canonical slot", peerSlot, kHostXferSlot);
    }

    // The live capture is unavailable (no live gamemode, unresolved save UFunctions, an unreadable
    // scratch): the canonical on-disk slot goes out behind the torn-read guard, since a stale world
    // still beats no world and the reconcile covers it.
    if (g_hostSlot.empty()) { ArmBeginNoSave_(peerSlot); return; }
    hs.active = true;  // TickHost::TryCaptureBlob_ captures the canonical slot
    UE_LOGW("save_transfer: slot %d -- LIVE capture unavailable; falling back to canonical "
            "slot '%ls' (stale; torn-read guard)", peerSlot, g_hostSlot.c_str());
}

void TickHost() {
    if (!g_session) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        HostStream& hs = g_host[slot];
        if (!hs.active) continue;
        if (!hs.blobReady) {
            TryCaptureBlob_(slot, hs);
            // The joiner is on a cover with nothing else to read: a capture that retries for its
            // stable read is working, and only this says so. No numerator -- the read restarts
            // whenever the file moves under it, so there is no honest fraction to report.
            coop::join_beacon::NotePhase(slot, coop::net::HostJoinPhase::CapturingWorld, 0, 0);
            continue;
        }
        // Begin first, success-gated: the pacing lane refuses under backpressure and the next tick
        // retries; no chunk moves until the announce is in the stream.
        if (!hs.beginSent) {
            if (!g_session->TrySendReliableToSlot(
                    slot, coop::net::ReliableKind::SaveTransferBegin,
                    &hs.beginPayload, sizeof(hs.beginPayload))) {
                // Backpressure (or slot dropped) -- retry next tick, but SAY SO. Falling through
                // to the `continue` alone left the slot emitting no beacon at all while the
                // announce waited, and the joiner's watchdog would then name a host that is alive
                // and correctly paced as one that stopped answering. Zero numerator, the same
                // shape the capture branch above uses: the announce has not moved yet.
                coop::join_beacon::NotePhase(slot, coop::net::HostJoinPhase::StreamingWorld, 0,
                                             static_cast<uint32_t>(hs.blob.size()));
                continue;
            }
            hs.beginSent = true;
        }
        // [dev] stall_world_stream_at_pct: stop handing chunks over once this much of the blob
        // has gone, and keep the connection and the beacon alive. It stages a download that STOPS
        // on a healthy link -- the one shape a fixed transfer cap used to answer by declaring the
        // host's save unavailable and booting a fresh world. Resolved once; a host knob, so a
        // joiner sees an ordinary host that went quiet mid-stream.
        static const long sStallPct =
            coop::config::ResolveInt(::coop::config_registry::rows::stall_world_stream_at_pct);
        const uint32_t stallAfter =
            sStallPct > 0 ? static_cast<uint32_t>(
                                (static_cast<uint64_t>(hs.chunkCount) * sStallPct) / 100)
                          : 0;
        if (stallAfter > 0 && hs.nextChunk >= stallAfter) {
            if (!hs.stallSaid) {
                hs.stallSaid = true;
                UE_LOGW("save_transfer: [dev] stall_world_stream_at_pct=%ld -- slot %d STALLED at "
                        "%u/%u chunks; the link stays up and the beacon keeps naming this phase",
                        sStallPct, slot, hs.nextChunk, hs.chunkCount);
            }
            continue;  // the beacon below is skipped too: a stalled stream reports no new number
        }
        for (int n = 0; n < kChunksPerTick && hs.nextChunk < hs.chunkCount; ++n) {
            const size_t off = static_cast<size_t>(hs.nextChunk) * coop::net::kSaveChunkBytes;
            const size_t dataLen =
                (hs.blob.size() - off < coop::net::kSaveChunkBytes)
                    ? hs.blob.size() - off
                    : coop::net::kSaveChunkBytes;
            // Game-thread-only scratch (TickHost is the sole writer); a 57 KB stack frame per pass
            // is avoidable.
            static uint8_t msg[4 + coop::net::kSaveChunkBytes];
            std::memcpy(msg, &hs.nextChunk, 4);
            std::memcpy(msg + 4, hs.blob.data() + off, dataLen);
            if (!g_session->TrySendReliableToSlot(
                    slot, coop::net::ReliableKind::SaveTransferChunk, msg,
                    static_cast<int>(4 + dataLen))) {
                break;  // send-buffer backpressure (or slot dropped) -- retry next tick
            }
            ++hs.nextChunk;
        }
        // Bytes this host has handed to the transport for this joiner, against the blob's size.
        // Its own quantity, not the joiner's: the two differ by whatever is in flight, which is
        // what a joiner comparing them can see for the first time.
        {
            const uint64_t handed =
                static_cast<uint64_t>(hs.nextChunk) * coop::net::kSaveChunkBytes;
            const uint64_t size = hs.blob.size();
            coop::join_beacon::NotePhase(slot, coop::net::HostJoinPhase::StreamingWorld,
                                         static_cast<uint32_t>(handed < size ? handed : size),
                                         static_cast<uint32_t>(size));
        }
        if (hs.nextChunk >= hs.chunkCount) {
            UE_LOGI("save_transfer: slot %d stream complete (%u chunks)", slot, hs.chunkCount);
            hs = HostStream{};  // frees the 17MB blob
        }
    }
    // The late flush of authoritative positions through each joiner's tail: its cadence is
    // this tick, so the join window's corrections ride the same pump as its blob.
    coop::join_window_baseline::TickLateArm();
}

void CancelForSlot(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    if (g_host[peerSlot].active)
        UE_LOGI("save_transfer: slot %d left mid-stream -- cancelled", peerSlot);
    g_host[peerSlot] = HostStream{};
    g_worldTaken[peerSlot] = false;
    coop::meadow_db_sync::CancelJoinSnapshot(peerSlot);  // drop the seed baseline and the pending masks
    coop::signal_sync::CancelJoinSnapshot(peerSlot);
    coop::email_sync::CancelJoinSnapshot(peerSlot);
    coop::join_window_baseline::ClearForSlot(peerSlot);  // the unconsumed baseline and its late flush
}

// The client.

void ClientArm() {
    std::lock_guard<std::mutex> lk(g_cliMu);
    g_cliState = ClientState::WaitingBegin;
    g_cliArmed = true;
    g_cliRequested = false;
    g_cliHaveBegin = false;
    g_cliChunksSeen = 0;
    g_cliTotal = g_cliChunkCount = g_cliCrc = 0;
    g_cliGameMode = 0;
    g_cliSidecarBytes = 0;
    g_cliBuf.clear();
    UE_LOGI("save_transfer: client ARMED (menu-mode join -- will request the host save)");
}

void ClientNoteConnected() {
    std::lock_guard<std::mutex> lk(g_cliMu);
    if (!g_cliArmed || g_cliRequested || !g_session) return;
    g_cliRequested = true;
    g_session->SendReliableToSlot(0, coop::net::ReliableKind::SaveTransferRequest, nullptr, 0);
    UE_LOGI("save_transfer: SaveTransferRequest sent to host");
}

void OnBegin(const coop::net::SaveTransferBeginPayload& p) {
    std::lock_guard<std::mutex> lk(g_cliMu);
    if (!g_cliArmed) return;
    // An ordinal past the enum is no mode, and the load would write it raw into the GameInstance.
    // Compatible peers share one enum (the join gate is byte-equality on the version pair), so
    // this is a corrupt or hostile header, not a mode to substitute for: read before the no-save
    // branch, since that branch boots a fresh world in the host's mode too.
    if (!ue_wrap::game_mode::IsValid(p.gameMode)) {
        UE_LOGE("save_transfer: Begin carries mode %u, which names no game mode -- protocol "
                "violation, failing", static_cast<unsigned>(p.gameMode));
        g_cliState = ClientState::Failed;
        return;
    }
    g_cliGameMode = p.gameMode;
    if (p.totalBytes == 0) {
        g_cliState = ClientState::NoSaveAvailable;
        UE_LOGW("save_transfer: host reports no save available -- falling back to a fresh world "
                "in the host's mode %u", static_cast<unsigned>(p.gameMode));
        return;
    }
    // Begin is once per arm: the flag clears only in ClientArm and the teardown, so a second Begin
    // is a protocol violation, and it used to reassign the scalars mid-transfer, moving the
    // completion denominator and the CRC out from under a stream in flight.
    if (g_cliHaveBegin) {
        UE_LOGE("save_transfer: second Begin during an active transfer (have total=%u, got %u) "
                "-- protocol violation, failing", g_cliTotal, p.totalBytes);
        g_cliState = ClientState::Failed;
        return;
    }
    g_cliHaveBegin = true;
    g_cliTotal = p.totalBytes;
    g_cliChunkCount = p.chunkCount;
    g_cliCrc = p.crc32;
    g_cliSidecarBytes = p.sidecarBytes;  // 0 = no sidecar
    if (g_cliState == ClientState::WaitingBegin) g_cliState = ClientState::Receiving;
    // No reserve here: totalBytes is an unvalidated wire u32, and reserving it let a hostile host
    // announce 4 GiB and kill the process with one packet. The reserve was only a hint; dropping it
    // costs a few vector doublings across a 17 MB download that takes seconds on the wire, and a
    // cap would have been a policy limit that could reject a legitimately grown world.
    UE_LOGI("save_transfer: Begin -- %u bytes in %u chunks (crc=0x%08X mode=%u sidecar=%u B)",
            p.totalBytes, p.chunkCount, p.crc32, p.gameMode, p.sidecarBytes);
    MaybeFinishLocked_();
}

ClientState GetClientState() {
    std::lock_guard<std::mutex> lk(g_cliMu);
    return g_cliState;
}

void GetProgress(uint32_t& doneBytes, uint32_t& totalBytes) {
    std::lock_guard<std::mutex> lk(g_cliMu);
    totalBytes = g_cliTotal;
    // A completed download reports its full size: the buffer clears once the slot is written while
    // the total stays, and a reader polling between the two once painted 0% for the whole world
    // load. Answered at the producer, where the state and the counter are under one lock.
    doneBytes = (g_cliState == ClientState::ReadySlotWritten)
                    ? g_cliTotal
                    : static_cast<uint32_t>(g_cliBuf.size());
}

uint8_t ReceivedGameMode() {
    std::lock_guard<std::mutex> lk(g_cliMu);
    return g_cliGameMode;
}

void CleanupStaleSlotsAtBoot() {
    const fs::path dir = coop::save_guard::SaveGamesDir();
    if (dir.empty()) return;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"zcoop_", 0) != 0) continue;
        // Age-gated: a concurrent same-machine sibling may be mid-join; only leftovers over an hour
        // old are swept.
        const auto mt = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        if (now - mt > std::chrono::hours(1)) DeleteFileLogged_(e.path());
    }
}

void OnDisconnect() {
    {
        std::lock_guard<std::mutex> lk(g_cliMu);
        g_cliState = ClientState::Idle;
        g_cliArmed = false;
        g_cliRequested = false;
        g_cliHaveBegin = false;
        g_cliChunksSeen = 0;
        g_cliSidecarBytes = 0;
        // The denominator dies with the buffer: a stale total left by a disconnect is a wrong
        // number for the first path that reads the progress before arming.
        g_cliTotal = 0;
        // The rest of the Begin's scalars go with it, so no field of a finished transfer outlives
        // its session; ClientArm re-zeroes the same set on the next join.
        g_cliChunkCount = 0;
        g_cliCrc = 0;
        g_cliGameMode = 0;
        g_cliBuf.clear();
        g_cliBuf.shrink_to_fit();
    }
    coop::save_identity_bind::OnDisconnect();  // drop the received map and the bound-native set
    const fs::path dir = coop::save_guard::SaveGamesDir();
    if (!dir.empty()) {
        DeleteFileLogged_(dir / (CoopSlotFileNameNoExt_() + L".sav"));
        DeleteFileLogged_(dir / (CoopSlotFileNameNoExt_() + L".sav.part"));
    }
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_host[slot] = HostStream{};
        g_worldTaken[slot] = false;
        coop::join_window_baseline::ClearForSlot(slot);  // no baseline survives a session end
    }
}

bool WorldTakenFor(int peerSlot) {
    return peerSlot >= 1 && peerSlot < coop::net::kMaxPeers && g_worldTaken[peerSlot];
}

}  // namespace coop::save_transfer
