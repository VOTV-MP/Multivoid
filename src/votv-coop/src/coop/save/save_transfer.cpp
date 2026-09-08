// coop/save_transfer.cpp -- the host's world to a joining client: a live capture of the host's
// world into a scratch slot (or the on-disk slot behind a torn-read guard), chunked over the
// bulk lane behind a Begin, CRC-checked and written to the client's coop slot; plus the
// save-time baselines (keys and positions) captured at the same instant, which the join
// reconcile reads. See coop/save_transfer.h.

#include "coop/save/save_transfer.h"

#include "coop/comms/chat_feed.h"  // the join-window cue, behind the pile_delta_probe flag
#include "coop/element/element.h"   // Element::GetActor
#include "coop/element/registry.h"  // the host's eid-to-actor lookup
#include "coop/config/config.h"  // the test-cue flag
#include "coop/interactables/meadow_db_sync.h"  // the join-seed multiset snapshot at the blob instant
#include "coop/interactables/signal_sync.h"  // the seed snapshot capture and cancel
#include "coop/world/email_sync.h"           // the seed snapshot capture and cancel
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"  // the blob-versus-live key diff
#include "coop/props/save_identity_bind.h"  // the client's eid-range bind
#include "coop/props/save_identity_map.h"  // the host's keyless index-to-eid map
#include "coop/save/save_guard.h"
#include "coop/save/save_indicator_suppress.h"  // suppress the SAVED HUD on the join scratch save
#include "ue_wrap/engine/engine.h"      // the host's current prop position
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"        // IsChipPile: a grabbed clump belongs to the convert stream
#include "ue_wrap/core/reflection.h"  // IsLive
#include "ue_wrap/engine/save_capture.h"

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
    std::vector<uint8_t> blob;     // captured stable blob (per-slot copy; 17MB,
                                   // freed on completion -- joins are rare)
};
HostStream g_host[coop::net::kMaxPeers];

// The keyed-prop keys the host's world held at the capture instant (the live-capture path only),
// which is what this joiner's blob contains. SendBlobDivergenceDeletes diffs it against the
// then-live set at the connect edge and sends an explicit PropDestroy per key the host has since
// removed (MTA's Packet_EntityRemove), instead of the divergence sweep inferring the delete.
// Outside HostStream, which is freed when the chunk stream completes, before the client finishes
// loading.
std::unordered_set<std::wstring> g_blobKeys[coop::net::kMaxPeers];

// The save-time position of every live keyless chipPile at the capture instant, by host eid: the
// positions the joiner loads its natives at. The connect replay stamps each pile's snapshot with
// it, so the client's twin destroy matches the save-loaded native at the old spot even when the
// host moved the pile in the join-load window. Same lifetime and threading as g_blobKeys.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobPileXforms[coop::net::kMaxPeers];

// The save-time position of every live off-form kerfur at the capture instant, by host eid; the
// host stamps it onto a KerfurConvert at a window turn-on so the client retires its stale local
// off-prop at the exact key. It outlives the snapshot (window turn-ons fire during the client's
// load tail) and clears at CancelForSlot, OnDisconnect and the late-flush expiry, the join
// window's true close; a later turn-on resolves by eid against an already-bound prop.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobKerfurXforms[coop::net::kMaxPeers];

// The save-time position of every live keyed prop at the capture instant, by host eid. A keyed
// prop rides the connect snapshot at the host's current position, but the joiner's own
// loadObjects re-creates it at the save position afterwards and clobbers that; the diverged
// flush re-asserts the host's live position at quiescence, past the clobber. Same lifetime and
// clears as the pile map.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobKeyedXforms[coop::net::kMaxPeers];

// Chunks per TickHost pass per slot: a ~13 MB/s ceiling at 60 Hz; the send buffer's backpressure
// (a failed send stops the pass) is the real pacer on a slower link.
constexpr int kChunksPerTick = 4;

// The late-armed flush: as a one-shot at the connect replay, a pile the host moved after that
// instant (a cluster cleared late in the joiner's long load tail) got no correction, its frozen
// save-position identity went stale and the position re-bind resurrected the old copy. The
// joiner's authoritative positions keep flushing for a window past world-ready, so every
// in-window move is delivered; a per-(slot, eid) last-sent position dedupes the wire to actual
// changes. The one-shot opens the window; TickHost re-runs on the cadence until it expires.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector> g_lastFlushedPilePos[coop::net::kMaxPeers];
// The keyed half's own last-sent dedupe, sharing the arm window.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector> g_lastFlushedKeyedPos[coop::net::kMaxPeers];
// The keyed scan reads GetActorLocation (a UFunction dispatch) for every keyed prop, about 2,000
// in a mature world, and at the pile cadence it hitched the host's game thread through the join
// tail. It runs on the first run (every already-moved prop) and then every Nth late-arm tick, a
// full scan each time, so a late move is still caught within a few seconds.
constexpr int kKeyedLateArmEvery = 5;   // 2 Hz / 5 = ~0.4 Hz keyed re-scan
int g_keyedLateArmTick[coop::net::kMaxPeers]{};
std::chrono::steady_clock::time_point g_pileFlushArmUntil[coop::net::kMaxPeers]{};
std::chrono::steady_clock::time_point g_pileFlushLastRun[coop::net::kMaxPeers]{};
constexpr auto kPileFlushLateWindow = std::chrono::seconds(25);       // cover a long load tail + late clusters
constexpr auto kPileFlushCadence    = std::chrono::milliseconds(500); // 2 Hz re-flush (cold; deduped to changes)

// A "no save" announce (zero bytes, zero chunks) as a pump stream, so TickHost delivers it with
// retry; a backpressure-deleted one left the client waiting forever.
void ArmBeginNoSave_(int slot) {
    HostStream& hs = g_host[slot];
    hs = HostStream{};
    hs.active = true;
    hs.blobReady = true;
    hs.beginSent = false;
    hs.beginPayload = coop::net::SaveTransferBeginPayload{};
    hs.beginPayload.totalBytes = 0;
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
    hs.nextChunk = 0;
    hs.chunkCount = static_cast<uint32_t>(
        (hs.blob.size() + coop::net::kSaveChunkBytes - 1) / coop::net::kSaveChunkBytes);

    // The announce is staged; TickHost sends it, with retry, before any chunk.
    hs.beginSent = false;
    hs.beginPayload = coop::net::SaveTransferBeginPayload{};
    hs.beginPayload.totalBytes = static_cast<uint32_t>(hs.blob.size());
    hs.beginPayload.chunkCount = hs.chunkCount;
    hs.beginPayload.crc32 = crc;
    hs.beginPayload.gameMode = 0;  // story, the coop target
    hs.beginPayload.sidecarBytes = sidecarBytes;  // the framed identity map leads the stream
    (void)slot;
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

void Install(coop::net::Session* session) {
    g_session = session;
    session->SetBulkSink(&BulkSink_);
    // The announce lands on the same thread as the payload it announces.
    session->SetSaveBeginSink(&BeginSink_);
}

// The host.

void SetHostSlot(const std::wstring& slot) {
    g_hostSlot = slot;
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
            // The keyed-prop set this blob contains (the host's live keyed props this instant),
            // diffed at the connect edge. Live-capture path only: the stale fallback leaves it
            // empty and the divergence sweep keeps full responsibility for that join.
            g_blobKeys[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedKeyedPropKeys(g_blobKeys[peerSlot]);
            // Every live keyless chipPile's save-time position at this same instant, for the
            // connect replay's match key; the stale fallback leaves it empty and the receiver uses
            // the live pose.
            g_blobPileXforms[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedPileTransforms(g_blobPileXforms[peerSlot]);
            // Every live off-form kerfur's save-time position at this same instant, for a window
            // turn-on's KerfurConvert.
            g_blobKerfurXforms[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedKerfurTransforms(g_blobKerfurXforms[peerSlot]);
            // Every live keyed prop's save-time position at this same instant, for the diverged
            // flush past the joiner's loadObjects clobber.
            g_blobKeyedXforms[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedKeyedPropTransforms(g_blobKeyedXforms[peerSlot]);
            // The meadow-DB content-hash multiset at this same instant; the ready-edge seed diffs
            // it against the then-live store. The stale fallback leaves it invalid, so no seed.
            coop::meadow_db_sync::CaptureJoinSnapshot(peerSlot);
            // The signal and email seeds capture at the same instant.
            coop::signal_sync::CaptureJoinSnapshot(peerSlot);
            coop::email_sync::CaptureJoinSnapshot(peerSlot);
            UE_LOGI("save_transfer: slot %d -- captured %zu keyed-prop keys + %zu pile + %zu kerfur + %zu keyed "
                    "save-time xforms at blob instant (R2 + Path 1c + scope A + F1 baselines)",
                    peerSlot, g_blobKeys[peerSlot].size(), g_blobPileXforms[peerSlot].size(),
                    g_blobKerfurXforms[peerSlot].size(), g_blobKeyedXforms[peerSlot].size());
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

void TickPileFlushLateArm();  // defined below; called at the tail of TickHost

void TickHost() {
    if (!g_session) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        HostStream& hs = g_host[slot];
        if (!hs.active) continue;
        if (!hs.blobReady) {
            TryCaptureBlob_(slot, hs);
            continue;
        }
        // Begin first, success-gated: the pacing lane refuses under backpressure and the next tick
        // retries; no chunk moves until the announce is in the stream.
        if (!hs.beginSent) {
            if (!g_session->TrySendReliableToSlot(
                    slot, coop::net::ReliableKind::SaveTransferBegin,
                    &hs.beginPayload, sizeof(hs.beginPayload))) {
                continue;  // backpressure (or slot dropped) -- retry next tick
            }
            hs.beginSent = true;
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
        if (hs.nextChunk >= hs.chunkCount) {
            UE_LOGI("save_transfer: slot %d stream complete (%u chunks)", slot, hs.chunkCount);
            hs = HostStream{};  // frees the 17MB blob
        }
    }
    TickPileFlushLateArm();  // the late flush of authoritative positions through each joiner's tail
}

void CancelForSlot(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    if (g_host[peerSlot].active)
        UE_LOGI("save_transfer: slot %d left mid-stream -- cancelled", peerSlot);
    g_host[peerSlot] = HostStream{};
    coop::meadow_db_sync::CancelJoinSnapshot(peerSlot);  // drop the seed baseline and the pending masks
    coop::signal_sync::CancelJoinSnapshot(peerSlot);
    coop::email_sync::CancelJoinSnapshot(peerSlot);
    g_blobKeys[peerSlot].clear();  // the unconsumed blob baseline
    g_blobPileXforms[peerSlot].clear();  // and the save-time maps
    g_blobKerfurXforms[peerSlot].clear();
    g_blobKeyedXforms[peerSlot].clear();
    g_pileFlushArmUntil[peerSlot] = {};        // disarm the late flush and drop its dedupe baselines
    g_lastFlushedPilePos[peerSlot].clear();
    g_lastFlushedKeyedPos[peerSlot].clear();
}

// The save-time position of pile `eid` for this joiner; none for a stale-fallback join or a pile
// unseeded at capture. Game thread.
bool TryGetSaveTimePileXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const auto& m = g_blobPileXforms[peerSlot];
    auto it = m.find(eid);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

// The pre-grab position of pile `eid` into every active join slot's map. Called at the grab's
// PRE edge, before the BP morphs the pile into a clump, so the position read here is the pile's
// save position, the key the joiner's native sits at; the landing convert carries it so the
// client arms a pending save-time twin. A slot is active while its map is non-empty, so outside
// a join this is a no-op. A re-grab overwrites.
void RecordGrabTimePileXform(coop::element::ElementId eid, const ue_wrap::FVector& preGrabLoc) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    int slots = 0;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_blobPileXforms[slot].empty()) continue;   // no active join captured piles here
        g_blobPileXforms[slot][eid] = preGrabLoc;
        ++slots;
    }
    if (slots > 0)
        UE_LOGI("[PILE-09] HOST pre-grab pos recorded eid=%u at (%.1f,%.1f,%.1f) -> %d active join slot(s) "
                "(the kToPile convert will carry it as the save-time key)",
                static_cast<unsigned>(eid), preGrabLoc.X, preGrabLoc.Y, preGrabLoc.Z, slots);
}

// Like TryGetSaveTimePileXform over all active slots: a convert is one fan-out with no slot, and a
// pile eid is unique, so at most one slot holds it.
bool TryGetSaveTimePileXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out) {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const auto& m = g_blobPileXforms[slot];
        auto it = m.find(eid);
        if (it != m.end()) { out = it->second; return true; }
    }
    return false;
}

// The diverged-position flush, one reconcile for every save-authoritative entity: a pile has no
// wire-position channel (both peers load it from the identical save), and a keyed prop rides the
// snapshot but the joiner's loadObjects re-creates it at the save position afterwards; either
// way a host move in the join window goes stale on the client. Per the joiner's save-time maps,
// the host's current position is compared and, where it diverged, a PropSnapPos is sent for the
// client to apply at quiescence, after its loadObjects. A pure position compare, independent of
// any convert's timing. The first run arms the late window and resets the dedupe; the cadence
// runs deliver only new moves.
void FlushDivergedSavePositionsForSlot_(int peerSlot, bool firstRun) {
    if (!g_session || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    const auto& pileM  = g_blobPileXforms[peerSlot];
    const auto& keyedM = g_blobKeyedXforms[peerSlot];
    if (pileM.empty() && keyedM.empty()) return;  // stale-fallback join (no save-time maps captured)
    if (firstRun) {
        g_lastFlushedPilePos[peerSlot].clear();
        g_lastFlushedKeyedPos[peerSlot].clear();
        g_keyedLateArmTick[peerSlot] = 0;
        g_pileFlushArmUntil[peerSlot] = std::chrono::steady_clock::now() + kPileFlushLateWindow;
    }
    constexpr float kDivergeCm2 = 4.0f * 4.0f;  // >4cm moved (above settle jitter) = a real in-window move
    constexpr float kResendCm2  = 4.0f * 4.0f;  // only re-send when the pos moved >4cm from what we last sent

    // The shared sender: a PropSnapPos at the actor's current transform, deduped per (slot, eid);
    // true when a correction went out. `kind` is a log tag.
    auto sendCorrection = [&](coop::element::ElementId eid, const ue_wrap::FVector& savePos, void* actor,
                              std::unordered_map<coop::element::ElementId, ue_wrap::FVector>& lastSent,
                              const char* kind) -> bool {
        const ue_wrap::FVector cur = ue_wrap::engine::GetActorLocation(actor);
        const float dx = cur.X - savePos.X, dy = cur.Y - savePos.Y, dz = cur.Z - savePos.Z;
        if (dx * dx + dy * dy + dz * dz <= kDivergeCm2) return false;  // unmoved (save IS current)
        if (auto it = lastSent.find(eid); it != lastSent.end()) {
            const float sx = cur.X - it->second.X, sy = cur.Y - it->second.Y, sz = cur.Z - it->second.Z;
            if (sx * sx + sy * sy + sz * sz <= kResendCm2) return false;  // already delivered @this pos
        }
        const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(actor);
        coop::net::PropSnapPosPayload p{};
        p.eid = static_cast<uint32_t>(eid);
        p.locX = cur.X; p.locY = cur.Y; p.locZ = cur.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        g_session->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropSnapPos, &p, sizeof(p));
        lastSent[eid] = cur;
        UE_LOGI("[PILE-B3] HOST slot %d %s pos-correction eid=%u save=(%.1f,%.1f,%.1f) -> current=(%.1f,%.1f,%.1f) "
                "drift=%.1fcm (%s in-window move -> deliver the authoritative position)",
                peerSlot, kind, static_cast<unsigned>(eid), savePos.X, savePos.Y, savePos.Z,
                cur.X, cur.Y, cur.Z, std::sqrt(dx * dx + dy * dy + dz * dz), firstRun ? "one-shot" : "late-arm");
        return true;
    };

    int sentPile = 0, checkedPile = 0;
    for (const auto& [eid, savePos] : pileM) {
        ++checkedPile;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->LiveActor() : nullptr;  // slot-validated
        if (!actor) continue;  // dead -> skip
        // Only a resting chipPile native gets a correction: a pile the host is grabbing or throwing
        // is a clump on an arc, which the convert stream owns, and chasing its waypoints armed
        // twins at airborne spots and doubled the pile.
        if (!ue_wrap::prop::IsChipPile(actor)) continue;  // grabbed clump / proxy -> the convert stream owns it
        if (sendCorrection(eid, savePos, actor, g_lastFlushedPilePos[peerSlot], "pile")) ++sentPile;
    }

    // The keyed scan runs on the first run, then every Nth late-arm tick.
    bool doKeyed = firstRun;
    if (!firstRun && !keyedM.empty() && ++g_keyedLateArmTick[peerSlot] >= kKeyedLateArmEvery) {
        g_keyedLateArmTick[peerSlot] = 0;
        doKeyed = true;
    }
    int sentKeyed = 0, checkedKeyed = 0;
    for (auto kit = keyedM.begin(); doKeyed && kit != keyedM.end(); ++kit) {
        const auto& eid = kit->first;
        const auto& savePos = kit->second;
        ++checkedKeyed;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->LiveActor() : nullptr;  // slot-validated
        if (!actor) continue;  // dead (e.g. hold-R pickup destroyed it) -> skip
        // The keyed path arms only the generic correction (the receiver's pile matcher matches
        // nothing for a keyed eid, so no twin and no dupe risk), and a host carrying the prop
        // mid-join sends transient positions that the late arm corrects, so no resting gate. A
        // keyed eid resolving to a pile is the pile map's row.
        if (ue_wrap::prop::IsChipPile(actor)) continue;
        if (sendCorrection(eid, savePos, actor, g_lastFlushedKeyedPos[peerSlot], "keyed")) ++sentKeyed;
    }

    if (sentPile > 0 || sentKeyed > 0 || firstRun)
        UE_LOGI("[PILE-B3] HOST slot %d diverged save-pos flush (%s) -- pile %d/%d + keyed %d/%d "
                "correction(s)/checked (connect-snapshot save-authoritative hole closed for piles AND keyed "
                "props; late-armed through the join tail)",
                peerSlot, firstRun ? "one-shot arm" : "late-arm tick",
                sentPile, checkedPile, sentKeyed, checkedKeyed);
}

void FlushDivergedSavePositionsForSlot(int peerSlot) {
    FlushDivergedSavePositionsForSlot_(peerSlot, /*firstRun=*/true);
}

// The host cadence: each armed joiner's authoritative positions keep flushing for the late window
// past its one-shot, so a move after the one-shot still arrives.
void TickPileFlushLateArm() {
    if (!g_session) return;
    const auto now = std::chrono::steady_clock::now();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pileFlushArmUntil[slot].time_since_epoch().count() == 0) continue;  // never armed
        if (now >= g_pileFlushArmUntil[slot]) {                                    // window expired -> disarm + free dedup
            g_pileFlushArmUntil[slot] = {};
            g_lastFlushedPilePos[slot].clear();
            g_lastFlushedKeyedPos[slot].clear();
            // The join window closes here for the slot. The save-time maps are join-window
            // structures, and with "active join" defined as a non-empty map that nothing emptied,
            // every steady-state host pile grab kept stamping its pre-grab key and every landing
            // carried one; the client then armed a hopeless pending twin per drop (its native was
            // already retired at the hand-off), and each pinned the quiescence drain for many
            // full-array sweeps, a hitch storm during pile play. The late flush is the maps' last
            // consumer: at its expiry the joiner has quiesced and reconciled, and a late kerfur
            // turn-on resolves by eid.
            if (!g_blobPileXforms[slot].empty() || !g_blobKerfurXforms[slot].empty() ||
                !g_blobKeyedXforms[slot].empty()) {
                UE_LOGI("[PILE-09] slot %d join window CLOSED (b3 late-flush expiry) -- retiring "
                        "save-time maps (%zu pile + %zu kerfur + %zu keyed xform(s)); steady-state grabs no "
                        "longer stamp save-time keys for this joiner",
                        slot, g_blobPileXforms[slot].size(), g_blobKerfurXforms[slot].size(),
                        g_blobKeyedXforms[slot].size());
                g_blobPileXforms[slot].clear();
                g_blobKerfurXforms[slot].clear();
                g_blobKeyedXforms[slot].clear();
            }
            continue;
        }
        if (now - g_pileFlushLastRun[slot] < kPileFlushCadence) continue;          // debounce to the cadence
        g_pileFlushLastRun[slot] = now;
        FlushDivergedSavePositionsForSlot_(slot, /*firstRun=*/false);
    }
}

bool TryGetSaveTimeKerfurXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out) {
    // A KerfurConvert is one fan-out with no slot, and a kerfur's host eid is unique, so all active
    // slots are searched.
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const auto& m = g_blobKerfurXforms[slot];
        auto it = m.find(eid);
        if (it != m.end()) { out = it->second; return true; }
    }
    return false;
}

void SendBlobDivergenceDeletes(int peerSlot) {
    if (!g_session || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    auto& blobKeys = g_blobKeys[peerSlot];
    if (blobKeys.empty()) return;  // no live-capture baseline (stale-fallback join) -- sweep owns it
    // The host's live keyed-prop set.
    std::unordered_set<std::wstring> liveKeys;
    coop::prop_element_tracker::CollectTrackedKeyedPropKeys(liveKeys);
    int sent = 0;
    for (const std::wstring& k : blobKeys) {
        if (liveKeys.count(k)) continue;  // still live -- the snapshot (re)asserts it
        // The blob had this prop and the host no longer does: named explicitly, so the joiner drops
        // exactly it.
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        for (size_t i = 0; i < k.size() && i < 31; ++i)
            dp.key.data[dp.key.len++] = static_cast<char>(k[i]);
        dp.elementId = 0;  // resolve by key: the eid is host-range-unstable across a transfer
        // Per slot: the divergence is this joiner's blob's. On the bulk lane ahead of the snapshot
        // bracket, so the removes land before the adds.
        g_session->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropDestroy,
                                      &dp, sizeof(dp));
        ++sent;
    }
    UE_LOGI("save_transfer: slot %d -- blob-vs-live diff sent %d explicit PropDestroy "
            "(blob had %zu keyed props, host live has %zu) [R2 MTA Packet_EntityRemove]",
            peerSlot, sent, blobKeys.size(), liveKeys.size());
    blobKeys.clear();
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
    if (p.totalBytes == 0) {
        g_cliState = ClientState::NoSaveAvailable;
        UE_LOGW("save_transfer: host reports no save available -- falling back to a fresh world");
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
    g_cliGameMode = p.gameMode;
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
        g_blobKeys[slot].clear();  // no blob baseline survives a session end
        g_blobPileXforms[slot].clear();  // nor the save-time maps
        g_blobKerfurXforms[slot].clear();
        g_blobKeyedXforms[slot].clear();
        g_pileFlushArmUntil[slot] = {};
        g_lastFlushedPilePos[slot].clear();
        g_lastFlushedKeyedPos[slot].clear();
    }
}

}  // namespace coop::save_transfer
