// coop/save_transfer.cpp -- see coop/save_transfer.h.

#include "coop/save_transfer.h"

#include "coop/net/session.h"
#include "coop/save_guard.h"
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
void BeginStreamFromBlob_(int slot, HostStream& hs, std::vector<uint8_t>&& bytes, uint32_t crc) {
    hs.active = true;
    hs.blob = std::move(bytes);
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
    const uint32_t crc = Crc32(g_cliBuf.data(), g_cliBuf.size());
    if (crc != g_cliCrc) {
        UE_LOGE("save_transfer: blob CRC mismatch (got 0x%08X want 0x%08X) -- failing",
                crc, g_cliCrc);
        g_cliState = ClientState::Failed;
        return;
    }
    const fs::path dir = coop::save_guard::SaveGamesDir();
    const fs::path tmp = dir / (CoopSlotFileNameNoExt_() + L".sav.part");
    const fs::path dst = dir / (CoopSlotFileNameNoExt_() + L".sav");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(g_cliBuf.data()),
                           static_cast<std::streamsize>(g_cliBuf.size()))) {
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
    UE_LOGI("save_transfer: host save written as '%ls' (%u bytes, crc ok) -- ready to load",
            dst.c_str(), g_cliTotal);
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
    if (ue_wrap::save_capture::CaptureLiveWorldToScratchSlot(kHostXferSlot)) {
        const fs::path scratchFile =
            coop::save_guard::SaveGamesDir() / (std::wstring(kHostXferSlot) + L".sav");
        std::vector<uint8_t> bytes;
        const bool got = ReadWholeFile(scratchFile, bytes) && !bytes.empty();
        DeleteFileLogged_(scratchFile);  // transient -- gone the instant it is read
        if (got) {
            const uint32_t crc = Crc32(bytes.data(), bytes.size());
            BeginStreamFromBlob_(peerSlot, hs, std::move(bytes), crc);
            UE_LOGI("save_transfer: slot %d streaming LIVE host world (%u bytes, %u chunks, "
                    "crc=0x%08X)",
                    peerSlot, static_cast<uint32_t>(hs.blob.size()), hs.chunkCount, crc);
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
    if (g_cliState == ClientState::WaitingBegin) g_cliState = ClientState::Receiving;
    g_cliBuf.reserve(p.totalBytes);
    UE_LOGI("save_transfer: Begin -- %u bytes in %u chunks (crc=0x%08X mode=%u)",
            p.totalBytes, p.chunkCount, p.crc32, p.gameMode);
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
        g_cliBuf.clear();
        g_cliBuf.shrink_to_fit();
    }
    const fs::path dir = coop::save_guard::SaveGamesDir();
    if (!dir.empty()) {
        DeleteFileLogged_(dir / (CoopSlotFileNameNoExt_() + L".sav"));
        DeleteFileLogged_(dir / (CoopSlotFileNameNoExt_() + L".sav.part"));
    }
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) g_host[slot] = HostStream{};
}

}  // namespace coop::save_transfer
