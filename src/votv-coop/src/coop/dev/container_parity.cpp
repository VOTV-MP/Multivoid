// coop/dev/container_parity.cpp -- see coop/dev/container_parity.h.

#include "coop/dev/container_parity.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace coop::dev::container_parity {
namespace {

using Clock = std::chrono::steady_clock;
namespace ccs = coop::props::container_contents_sync;

constexpr uint32_t kReportBytes = 4;   // the blob's u32 count
constexpr auto kAssemblyTtl = std::chrono::seconds(10);
constexpr unsigned kMaxFailLines = 20;

std::atomic<bool> g_enabled{false};
coop::blob_chunks::Assembler g_asm;
std::deque<Report> g_reports;
uint32_t g_seq = 0;
Clock::time_point g_nextSweep{};

size_t MaxRows() {
    return (coop::blob_chunks::MaxBlobBytes() - kReportBytes) / sizeof(coop::net::DevContainerParityEntry);
}

bool RedArm() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::container_parity_red);
    return s;
}

// One row per world container this peer can read now. A container mid-transfer or unreadable has no row.
void Gather(uint32_t eid, void* ctx) {
    uint64_t hash = 0;
    uint32_t records = 0;
    if (!ccs::ContentHashOf(eid, hash, records)) return;
    static_cast<std::vector<coop::net::DevContainerParityEntry>*>(ctx)->push_back({eid, records, hash});
}

std::vector<coop::net::DevContainerParityEntry> GatherAll() {
    std::vector<coop::net::DevContainerParityEntry> rows;
    ccs::ForEachWorldContainer(&Gather, &rows);
    return rows;
}

}  // namespace

void Enable() { g_enabled.store(true, std::memory_order_release); }

bool SendReport(coop::net::Session& session) {
    std::vector<coop::net::DevContainerParityEntry> rows = GatherAll();
    if (RedArm() && !rows.empty()) {
        rows.front().contentHash ^= 1ull;
        UE_LOGI("[CONTAINER-PARITY] client RED ARM: eid=%u hash perturbed", rows.front().eid);
    }
    if (rows.size() > MaxRows()) {
        UE_LOGW("[CONTAINER-PARITY] the report of %zu rows passes the transport ceiling of %zu", rows.size(), MaxRows());
        return false;
    }
    std::vector<uint8_t> blob(kReportBytes + rows.size() * sizeof(coop::net::DevContainerParityEntry));
    const uint32_t count = static_cast<uint32_t>(rows.size());
    std::memcpy(blob.data(), &count, sizeof(count));
    if (!rows.empty())
        std::memcpy(blob.data() + kReportBytes, rows.data(),
                    rows.size() * sizeof(coop::net::DevContainerParityEntry));
    return coop::blob_chunks::SendBlob(&session, coop::net::ReliableKind::DevContainerParity, ++g_seq, blob);
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(chunk, senderSlot, blob)) return;
    uint32_t count = 0;
    if (blob.size() < kReportBytes) {
        UE_LOGW("[CONTAINER-PARITY] a report of %zu bytes from slot %u is shorter than its count -- dropping",
                blob.size(), static_cast<unsigned>(senderSlot));
        return;
    }
    std::memcpy(&count, blob.data(), sizeof(count));
    if (count > MaxRows() ||
        blob.size() != kReportBytes + static_cast<size_t>(count) * sizeof(coop::net::DevContainerParityEntry)) {
        UE_LOGW("[CONTAINER-PARITY] a report from slot %u names %u rows in %zu bytes -- dropping",
                static_cast<unsigned>(senderSlot), count, blob.size());
        return;
    }
    Report r;
    r.slot = senderSlot;
    r.rows.resize(count);
    if (count)
        std::memcpy(r.rows.data(), blob.data() + kReportBytes,
                    count * sizeof(coop::net::DevContainerParityEntry));
    g_reports.push_back(std::move(r));
}

bool TakeReport(Report& out) {
    if (g_reports.empty()) return false;
    out = std::move(g_reports.front());
    g_reports.pop_front();
    return true;
}

bool JudgeReport(const Report& report, std::string& firstFail) {
    firstFail.clear();
    const unsigned slot = static_cast<unsigned>(report.slot);
    const std::vector<coop::net::DevContainerParityEntry> mine = GatherAll();
    std::unordered_map<uint32_t, const coop::net::DevContainerParityEntry*> hostBy;
    hostBy.reserve(mine.size());
    for (const auto& e : mine) hostBy[e.eid] = &e;

    size_t mismatched = 0, missing = 0, extra = 0, failLines = 0;
    auto fail = [&](const char* fmt, auto... args) {
        char line[256];
        std::snprintf(line, sizeof(line), fmt, args...);
        if (firstFail.empty()) firstFail = line;
        if (failLines++ < kMaxFailLines) UE_LOGW("%s", line);
    };

    std::unordered_set<uint32_t> reported;
    reported.reserve(report.rows.size());
    for (const auto& j : report.rows) {
        reported.insert(j.eid);
        const auto it = hostBy.find(j.eid);
        if (it == hostBy.end()) {
            ++extra;
            fail("[CONTAINER-PARITY] host FAIL: slot %u eid=%u is not a world container on the host", slot, j.eid);
            continue;
        }
        if (it->second->contentHash != j.contentHash) {
            ++mismatched;
            fail("[CONTAINER-PARITY] host FAIL: slot %u eid=%u records host=%u joiner=%u -- the contents differ",
                 slot, j.eid, it->second->records, j.records);
        }
    }
    for (const auto& h : mine) {
        if (reported.count(h.eid)) continue;
        ++missing;
        fail("[CONTAINER-PARITY] host FAIL: slot %u lacks eid=%u (records %u)", slot, h.eid, h.records);
    }
    if (failLines > kMaxFailLines)
        UE_LOGW("[CONTAINER-PARITY] ... and %zu more", failLines - kMaxFailLines);
    UE_LOGI("[CONTAINER-PARITY] host slot %u n=%zu mismatched=%zu missing=%zu extra=%zu", slot,
            report.rows.size(), mismatched, missing, extra);
    return mismatched == 0 && missing == 0 && extra == 0;
}

void Tick() {
    if (g_asm.Idle()) return;
    const auto now = Clock::now();
    if (now < g_nextSweep) return;
    g_nextSweep = now + std::chrono::seconds(1);
    g_asm.Sweep(now, kAssemblyTtl);
}

void OnDisconnect() {
    g_asm.Clear();
    g_reports.clear();
    g_nextSweep = {};
}

}  // namespace coop::dev::container_parity
