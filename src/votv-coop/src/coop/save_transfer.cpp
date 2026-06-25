// coop/save_transfer.cpp -- see coop/save_transfer.h.

#include "coop/save_transfer.h"

#include "coop/chat_feed.h"  // v86 Path 1c hands-on: in-game JOIN-WINDOW OPEN cue (gated on pile_delta_probe)
#include "coop/ini_config.h"  // IsIniKeyTrue -- the hands-on test-cue gate
#include "coop/net/session.h"
#include "coop/prop_element_tracker.h"  // R2: CollectTrackedKeyedPropKeys (blob-vs-live diff)
#include "coop/save_identity_bind.h"  // Phase 2b: client eid-range bind (SetReceivedMap / OnDisconnect)
#include "coop/save_identity_map.h"  // Phase 1B: host-side keyless index->eid map build + log (gated, no wire)
#include "coop/save_guard.h"
#include "coop/save_indicator_suppress.h"  // detect/suppress the SAVED HUD on join-save
#include "ue_wrap/log.h"
#include "ue_wrap/save_capture.h"

#include <windows.h>

#include <atomic>
#include <chrono>
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

// ---- CRC-32 (IEEE, table-driven; both sides of the wire use this) --------------
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

// ---- HOST side (game thread only: OnRequest / TickHost / CancelForSlot) --------

std::wstring g_hostSlot;  // the slot the host's world was loaded from

// Torn-read guard state (design-workflow B1): VOTV's saveToSlot writes the .sav
// IN PLACE (no temp+rename), so a join landing mid-write would read a torn blob.
// The file is trusted only when (size, mtime) is STABLE across two TickHost polls
// >= kStablePollMs apart AND two consecutive full reads CRC-identical.
constexpr uint64_t kStablePollMs = 300;

struct HostStream {
    bool     active = false;
    bool     blobReady = false;
    uint32_t nextChunk = 0;
    uint32_t chunkCount = 0;
    // stable-read probe state
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

// R2 (2026-06-17): the keyed-prop wire-keys the host's world held at blob-capture
// instant (OnRequest, LIVE-capture path only) == what this joiner's blob contains.
// SendBlobDivergenceDeletes (connect edge) diffs it against the then-live set and
// sends EXPLICIT per-key PropDestroy for props the blob HAD that the host has since
// removed -- so the client drops exactly those (MTA Packet_EntityRemove) instead of
// the divergence sweep INFERRING the delete (where every kerfur-dupe regression
// lived). Lives OUTSIDE HostStream because HostStream is freed when the chunk stream
// completes (TickHost), which is BEFORE the client finishes loading + announces ready.
std::unordered_set<std::wstring> g_blobKeys[coop::net::kMaxPeers];

// v86 Path 1c: the SAVE-TIME position of every live keyless chipPile at the blob
// instant, keyed by host eid. Captured in OnRequest right after the scratch save is
// serialized (== the positions the joining client loads its natives at). The connect
// replay (prop_snapshot::BuildPropSpawnPayload_) reads it via TryGetSaveTimePileXform
// to stamp each pile's join snapshot with its save-time key, so the client twin-destroy
// matches the save-loaded native at @old even when the host MOVED the pile @new in the
// join-load window (the two-channel DUP fix). Same lifetime/threading as g_blobKeys.
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobPileXforms[coop::net::kMaxPeers];

// scope A (kerfur off->active dup retire, 2026-06-24): the SAVE-TIME position of every live OFF-form
// kerfur at the blob instant, keyed by host eid. Captured alongside g_blobPileXforms (same OnRequest
// instant == the positions the joiner loads its kerfur natives at). Read via
// TryGetSaveTimeKerfurXformAnySlot at BindFormActor (the host stamps it onto the KerfurConvert when it
// turns a kerfur ON in the join window) so the client retires its stale local off-prop at the exact key.
// Same lifetime/threading as g_blobPileXforms -- it OUTLIVES the snapshot (window turn-ons fire during
// the client load-tail) and is cleared only at CancelForSlot / OnDisconnect (bounded; no per-session leak).
std::unordered_map<coop::element::ElementId, ue_wrap::FVector>
    g_blobKerfurXforms[coop::net::kMaxPeers];

// How many chunks one TickHost pass may push per slot. 4 x 56KB at 60 Hz = ~13
// MB/s ceiling; the GNS send buffer's backpressure (send failure -> stop, retry
// next tick) is the real pacer on slower links.
constexpr int kChunksPerTick = 4;

void SendBeginNoSave_(int slot) {
    coop::net::SaveTransferBeginPayload b{};
    b.totalBytes = 0;
    g_session->SendReliableToSlot(slot, coop::net::ReliableKind::SaveTransferBegin,
                                  &b, sizeof(b));
    UE_LOGW("save_transfer: no readable host save for slot %d -- client will fresh-boot", slot);
}

// Stamp a captured blob into the host stream + announce it (SaveTransferBegin). The
// TickHost chunk pump streams it from there. Shared by the LIVE-capture path
// (OnRequest) and the canonical torn-read fallback (TryCaptureBlob_); `crc` is
// precomputed by the caller (both already have it in hand).
void BeginStreamFromBlob_(int slot, HostStream& hs, std::vector<uint8_t>&& bytes, uint32_t crc,
                          uint32_t sidecarBytes = 0) {
    hs.active = true;
    hs.blob = std::move(bytes);  // already framed (identity sidecar prepended) by the caller
    hs.blobReady = true;
    hs.nextChunk = 0;
    hs.chunkCount = static_cast<uint32_t>(
        (hs.blob.size() + coop::net::kSaveChunkBytes - 1) / coop::net::kSaveChunkBytes);

    coop::net::SaveTransferBeginPayload b{};
    b.totalBytes = static_cast<uint32_t>(hs.blob.size());
    b.chunkCount = hs.chunkCount;
    b.crc32 = crc;
    b.gameMode = 0;  // story -- the coop target; a sandbox-host variant threads the
                     // live GameMode here when sandbox coop becomes a goal
    b.sidecarBytes = sidecarBytes;  // Phase 2: leading bytes of the stream that are the framed identity map
    g_session->SendReliableToSlot(slot, coop::net::ReliableKind::SaveTransferBegin,
                                  &b, sizeof(b));
}

// One stable-read attempt for a slot still capturing its blob. Returns true when
// the blob is captured (Begin sent), false to retry next tick.
bool TryCaptureBlob_(int slot, HostStream& hs) {
    const uint64_t now = ::GetTickCount64();
    if (now - hs.lastProbeTick < kStablePollMs) return false;  // wait out the poll gap
    hs.lastProbeTick = now;

    const fs::path file = coop::save_guard::SaveGamesDir() / (g_hostSlot + L".sav");
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    if (ec) {
        if (++hs.readAttempts >= 4) { SendBeginNoSave_(slot); hs.active = false; }
        return false;
    }
    const auto mtime = fs::last_write_time(file, ec).time_since_epoch().count();
    if (ec) return false;

    if (size != hs.lastSize || mtime != hs.lastMtime) {
        // Changed since the last probe -- the game may be mid-save. Restart the
        // stability count (this also covers the very first probe).
        hs.lastSize = size;
        hs.lastMtime = mtime;
        hs.stableCount = 1;
        hs.haveFirstRead = false;
        return false;
    }
    if (++hs.stableCount < 3) return false;  // need 2 stable gaps (3 identical probes)

    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(file, bytes) || bytes.empty()) {
        if (++hs.readAttempts >= 4) { SendBeginNoSave_(slot); hs.active = false; }
        return false;
    }
    const uint32_t crc = Crc32(bytes.data(), bytes.size());
    if (!hs.haveFirstRead) {
        // First full read: remember its CRC, require the NEXT read to match
        // (double-read compare closes the in-place-write torn window).
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

// ---- CLIENT side (net thread sink + game thread OnBegin + timeline poll) -------

std::mutex g_cliMu;
ClientState g_cliState = ClientState::Idle;
bool     g_cliArmed = false;
bool     g_cliRequested = false;
uint32_t g_cliTotal = 0;
uint32_t g_cliChunkCount = 0;
uint32_t g_cliCrc = 0;
uint8_t  g_cliGameMode = 0;
uint32_t g_cliSidecarBytes = 0;  // Phase 2: leading bytes of g_cliBuf that are the framed identity map
bool     g_cliHaveBegin = false;
uint32_t g_cliChunksSeen = 0;
std::vector<uint8_t> g_cliBuf;

std::wstring CoopSlotFileNameNoExt_() {
    wchar_t buf[32];
    swprintf(buf, 32, L"zcoop_%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
    return buf;
}

// Both completion inputs (Begin via game thread, bytes via net thread) funnel
// here under g_cliMu: when the blob is whole, CRC-verify and write the slot.
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
    // Phase 2a: the host may have PREPENDED the framed {index->eid} identity sidecar (sidecarBytes>0). It is
    // framing metadata, NOT part of the .sav -- strip it so the game's loadObjects sees a clean GVAS blob. The
    // split length (sidecarBytes) comes from the CRC-verified Begin payload, so it's authoritative even if the
    // map's inner framing is malformed; parse + log the map (NO bind -- that is step 2b) best-effort.
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
            coop::save_identity_map::LogReceivedMap(rxMap);  // Phase 2a transport checkpoint -- log
            // Phase 2b: hand the map to the eid-range bind + arm it BEFORE the harness loads the slot (the
            // natives spawn during the subsequent loadObjects). No-op unless [dev] save_identity_bind=1.
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

// NET THREAD: registered as the session's bulk sink. Chunk payload = u32 idx +
// raw bytes; chunks of one stream arrive in-lane-order through the single net
// thread, so a sequential index check suffices.
void BulkSink_(int senderPeerSlot, const uint8_t* data, int len) {
    if (senderPeerSlot != 0) return;  // host-originated only
    if (len < 4) return;
    uint32_t idx = 0;
    std::memcpy(&idx, data, 4);
    std::lock_guard<std::mutex> lk(g_cliMu);
    if (g_cliState != ClientState::WaitingBegin && g_cliState != ClientState::Receiving) return;
    if (g_cliState == ClientState::WaitingBegin) g_cliState = ClientState::Receiving;
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
}

// ---- HOST ----------------------------------------------------------------------

void SetHostSlot(const std::wstring& slot) {
    g_hostSlot = slot;
    UE_LOGI("save_transfer: host slot = '%ls'", slot.c_str());
}

// The throwaway slot save_capture serializes the LIVE host world into. The zcoop_
// prefix means CleanupStaleSlotsAtBoot already sweeps a crash leftover; we also
// delete it the instant we've read it below, so it normally never lingers. It is
// NEVER the host's canonical slot -- the host's progress is untouchable here.
constexpr const wchar_t* kHostXferSlot = L"zcoop_hostxfer";

void OnRequest(int peerSlot) {
    if (!g_session || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    HostStream& hs = g_host[peerSlot];
    hs = HostStream{};  // reset any prior stream for this slot (rejoin)

    // v86 Path 1c hands-on cue (gated on the pile_delta_probe test flag, OFF in normal play): the joiner
    // just requested the save -- the JOIN-WINDOW is now OPEN. A host pile MOVED from here until the
    // joiner hits world-ready ([PILE-1C] / "JOIN-WINDOW CLOSED") is in-window and its save-time key
    // reconciles the client native. Drop the test piles AFTER this line appears, BEFORE the CLOSED cue.
    if (coop::ini_config::IsIniKeyTrue("pile_delta_probe"))
        coop::chat_feed::Push(L"[1c-test] JOIN-WINDOW OPEN -- joiner loading; move/drop test piles NOW (close at 'JOIN-WINDOW CLOSED')");

    // ROOT-CAUSE FIX (2026-06-15): serialize the host's world LIVE, right now, into
    // a throwaway scratch slot -- instead of shipping the stale on-disk .sav. An
    // entity the host changed since its last autosave (the canonical case: a kerfur
    // turned ON -> now an NPC) is captured in its live state, so the joiner's native
    // loadObjects() builds a correct world and the reconcile layer has nothing to
    // fix up (no ghost off-prop, no fresh-spawn of an already-loaded NPC). The
    // canonical slot is never named or written (save_capture controls the slot name)
    // -- this is not a real save and cannot clobber host progress. We are on the game
    // thread (net_pump::Tick asserts it), so the save UFunctions are legal here; the
    // scratch file is read synchronously (no torn-read window -- we just wrote it)
    // then deleted.
    // Suppress the native "SAVED..." HUD ONLY for THIS join scratch-save: bracket the
    // capture so the saveAnim/addHint indicator (fired BP-internally inside saveObjects)
    // is detected here (this build: log-only -> later no-op). A manual F5/menu save uses
    // the BP save()/autosave() path the mod never calls, so its SAVED is untouched. The
    // bracket is the coop call site (NOT inside ue_wrap save_capture -> layering); it
    // spans the entire synchronous capture, the tight window the indicator fires in.
    coop::save_indicator_suppress::Begin();
    const bool captured = ue_wrap::save_capture::CaptureLiveWorldToScratchSlot(kHostXferSlot);
    coop::save_indicator_suppress::End();
    if (captured) {
        const fs::path scratchFile =
            coop::save_guard::SaveGamesDir() / (std::wstring(kHostXferSlot) + L".sav");
        std::vector<uint8_t> bytes;
        const bool got = ReadWholeFile(scratchFile, bytes) && !bytes.empty();
        DeleteFileLogged_(scratchFile);  // transient -- gone the instant it is read
        if (got) {
            // Phase 2a (stable-id identity sidecar transport; gated dev checkpoint, NO bind yet): build the
            // {objectsData-index -> host-eid} map for the keyless save-loaded natives and PREPEND it to the
            // blob, so it travels ATOMICALLY inside the same CRC'd stream the client loads (one stream, one
            // CRC -- the map cannot desync from the blob it indexes). The client strips + logs it (step 2a);
            // the BindLocalNativeToHostEid consumer lands in step 2b. [dev] save_identity_map_log=1 only;
            // absent (the shipping default + the user's normal play) -> sidecarBytes=0 -> byte-identical to
            // the pre-sidecar stream. Built BEFORE BeginStreamFromBlob_ so the framing is part of the CRC.
            std::vector<uint8_t> sidecar;  // empty unless the dev flag is on (then 12B header + 9B/entry)
            if (coop::ini_config::IsIniKeyTrue("save_identity_map_log")) {
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
            // R2 baseline: record the keyed-prop set this blob contains (== the host's
            // live keyed props this instant -- the blob was just serialized from them).
            // SendBlobDivergenceDeletes diffs it at the connect edge. LIVE-capture path
            // only; the stale-fallback below leaves g_blobKeys empty so the divergence
            // sweep keeps full responsibility for that join (clean degradation).
            g_blobKeys[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedKeyedPropKeys(g_blobKeys[peerSlot]);
            // v86 Path 1c: capture every live keyless chipPile's SAVE-TIME position (keyed by host
            // eid) at this SAME blob instant -- the positions the joining client loads its natives at.
            // The connect replay stamps each pile's snapshot with its key so the client twin-destroy
            // reconciles a host-moved-in-window pile (native@old vs save-time key@old). LIVE-capture
            // path only (the stale-fallback leaves it empty -> the receiver uses the live pose).
            g_blobPileXforms[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedPileTransforms(g_blobPileXforms[peerSlot]);
            // scope A: capture every live OFF-form kerfur's save-time position (keyed by host eid) at this
            // SAME blob instant -- the positions the joiner loads its kerfur natives at. The host stamps it
            // onto the KerfurConvert at a window turn-on so the client retires its stale local off-prop.
            g_blobKerfurXforms[peerSlot].clear();
            coop::prop_element_tracker::CollectTrackedKerfurTransforms(g_blobKerfurXforms[peerSlot]);
            UE_LOGI("save_transfer: slot %d -- captured %zu keyed-prop keys + %zu pile + %zu kerfur "
                    "save-time xforms at blob instant (R2 + Path 1c + scope A baselines)",
                    peerSlot, g_blobKeys[peerSlot].size(), g_blobPileXforms[peerSlot].size(),
                    g_blobKerfurXforms[peerSlot].size());
            return;
        }
        UE_LOGW("save_transfer: slot %d -- live scratch '%ls.sav' unreadable after capture; "
                "falling back to the canonical slot", peerSlot, kHostXferSlot);
    }

    // Live capture unavailable (gamemode not live / save UFunctions unresolved /
    // scratch unreadable -- a should-never-happen while a client is mid-join). Degrade
    // to the canonical on-disk slot via TickHost's torn-read guard rather than deny
    // the join: a stale world still beats no world (the reconcile layer covers it, as
    // it did before this fix).
    if (g_hostSlot.empty()) { SendBeginNoSave_(peerSlot); return; }
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
            continue;
        }
        for (int n = 0; n < kChunksPerTick && hs.nextChunk < hs.chunkCount; ++n) {
            const size_t off = static_cast<size_t>(hs.nextChunk) * coop::net::kSaveChunkBytes;
            const size_t dataLen =
                (hs.blob.size() - off < coop::net::kSaveChunkBytes)
                    ? hs.blob.size() - off
                    : coop::net::kSaveChunkBytes;
            // Game-thread-only scratch (TickHost is the sole writer) -- a 57KB
            // stack frame per pass is avoidable.
            static uint8_t msg[4 + coop::net::kSaveChunkBytes];
            std::memcpy(msg, &hs.nextChunk, 4);
            std::memcpy(msg + 4, hs.blob.data() + off, dataLen);
            if (!g_session->SendReliableToSlot(
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
}

void CancelForSlot(int peerSlot) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    if (g_host[peerSlot].active)
        UE_LOGI("save_transfer: slot %d left mid-stream -- cancelled", peerSlot);
    g_host[peerSlot] = HostStream{};
    g_blobKeys[peerSlot].clear();  // R2: drop any unconsumed blob baseline
    g_blobPileXforms[peerSlot].clear();  // v86 Path 1c: drop the unconsumed save-time pile map
    g_blobKerfurXforms[peerSlot].clear();  // scope A: drop the unconsumed save-time kerfur map
}

// v86 Path 1c: the save-time position of pile `eid` captured at the blob instant for
// `peerSlot` (empty for the stale-fallback join, or for a pile that was unseeded at
// capture / spawned after the save). Game thread (the connect replay builder calls it).
bool TryGetSaveTimePileXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out) {
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const auto& m = g_blobPileXforms[peerSlot];
    auto it = m.find(eid);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

// docs/piles/09: record the PRE-GRAB position of pile `eid` into every ACTIVE join slot's
// blob pile map. Called from OnPileGrabPre (host, game thread) at the InpActEvt PRE edge,
// immediately BEFORE the BP morphs the pile -> clump -- so the position read here == the
// pile's save/native position (it has not moved yet), == the exact key the joining client's
// native@old sits at. By stamping it into g_blobPileXforms[slot][eid], the kToPile LAND
// convert (BroadcastConvert) reads it via TryGetSaveTimePileXformAnySlot and carries it so the
// client arms a pending save-time twin. A slot is "active" iff its blob map is non-empty (a
// join captured piles into it); outside a join no slot qualifies -> no-op (the fix is scoped
// to the join window). Idempotent: a re-grab of the same eid overwrites with the latest pos.
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

// docs/piles/09: like TryGetSaveTimePileXform but searches ALL active join slots. BroadcastConvert
// is a single fan-out (not per-joiner), so it can't pass a slot -- same shape as
// TryGetSaveTimeKerfurXformAnySlot. A pile eid is unique, so at most one slot holds it.
bool TryGetSaveTimePileXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out) {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const auto& m = g_blobPileXforms[slot];
        auto it = m.find(eid);
        if (it != m.end()) { out = it->second; return true; }
    }
    return false;
}

bool TryGetSaveTimeKerfurXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out) {
    // The host turn-on broadcast (KerfurConvert) is a single fan-out, not per-joiner, and a kerfur
    // off-prop's host eid is unique -- so search all active slots' blob maps and take the first hit.
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
    // Current live keyed-prop set on the host.
    std::unordered_set<std::wstring> liveKeys;
    coop::prop_element_tracker::CollectTrackedKeyedPropKeys(liveKeys);
    int sent = 0;
    for (const std::wstring& k : blobKeys) {
        if (liveKeys.count(k)) continue;  // still live -- the snapshot (re)asserts it
        // The blob HAD this prop; the host's live world no longer does. Name it
        // explicitly so the joiner drops exactly it -- no divergence-sweep inference.
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        for (size_t i = 0; i < k.size() && i < 31; ++i)
            dp.key.data[dp.key.len++] = static_cast<char>(k[i]);
        dp.elementId = 0;  // resolve by key: the eid is host-range-unstable across a transfer
        // Per-slot (NOT broadcast): this divergence is specific to THIS joiner's blob.
        // Bulk lane, ahead of the snapshot bracket (caller orders us before
        // TriggerForSlot) so the removes land before the adds.
        g_session->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropDestroy,
                                      &dp, sizeof(dp));
        ++sent;
    }
    UE_LOGI("save_transfer: slot %d -- blob-vs-live diff sent %d explicit PropDestroy "
            "(blob had %zu keyed props, host live has %zu) [R2 MTA Packet_EntityRemove]",
            peerSlot, sent, blobKeys.size(), liveKeys.size());
    blobKeys.clear();
}

// ---- CLIENT --------------------------------------------------------------------

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

bool ClientArmed() {
    std::lock_guard<std::mutex> lk(g_cliMu);
    return g_cliArmed;
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
    g_cliHaveBegin = true;
    g_cliTotal = p.totalBytes;
    g_cliChunkCount = p.chunkCount;
    g_cliCrc = p.crc32;
    g_cliGameMode = p.gameMode;
    g_cliSidecarBytes = p.sidecarBytes;  // Phase 2: leading framed-identity-map bytes (0 = no sidecar)
    if (g_cliState == ClientState::WaitingBegin) g_cliState = ClientState::Receiving;
    g_cliBuf.reserve(p.totalBytes);
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
    doneBytes = static_cast<uint32_t>(g_cliBuf.size());
    totalBytes = g_cliTotal;
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
        // Age gate: a CONCURRENT same-machine sibling may be mid-join right now;
        // only crash leftovers (>1 h) are swept.
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
        g_cliBuf.clear();
        g_cliBuf.shrink_to_fit();
    }
    coop::save_identity_bind::OnDisconnect();  // Phase 2b: drop the received map + bound-native guard set
    const fs::path dir = coop::save_guard::SaveGamesDir();
    if (!dir.empty()) {
        DeleteFileLogged_(dir / (CoopSlotFileNameNoExt_() + L".sav"));
        DeleteFileLogged_(dir / (CoopSlotFileNameNoExt_() + L".sav.part"));
    }
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_host[slot] = HostStream{};
        g_blobKeys[slot].clear();  // R2: no blob baseline survives a session end
        g_blobPileXforms[slot].clear();  // v86 Path 1c: nor the save-time pile map
        g_blobKerfurXforms[slot].clear();  // scope A: nor the save-time kerfur map
    }
}

}  // namespace coop::save_transfer
