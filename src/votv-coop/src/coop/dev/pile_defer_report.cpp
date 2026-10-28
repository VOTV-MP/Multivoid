// coop/dev/pile_defer_report.cpp -- see coop/dev/pile_defer_report.h.

#include "coop/dev/pile_defer_report.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <string>

namespace coop::dev::pile_defer_report {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kReportBytes = 4;   // the blob's u32 count
constexpr auto kAssemblyTtl = std::chrono::seconds(10);

coop::blob_chunks::Assembler g_asm;
std::deque<Report> g_reports;
uint32_t g_seq = 0;
Clock::time_point g_nextSweep{};

size_t MaxRows() {
    return (coop::blob_chunks::MaxBlobBytes() - kReportBytes) / sizeof(coop::net::DevParityEntry);
}

}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::pile_defer_drill);
        return v == "check" ? Arm::Check : v == "giveup" ? Arm::Giveup : Arm::Off;
    }();
    return a;
}

bool Send(coop::net::Session& session, const std::vector<coop::net::DevParityEntry>& rows) {
    if (rows.size() > MaxRows()) {
        UE_LOGW("[PILE-DEFER] the parity report of %zu rows passes the transport ceiling of %zu", rows.size(), MaxRows());
        return false;
    }
    std::vector<uint8_t> blob(kReportBytes + rows.size() * sizeof(coop::net::DevParityEntry));
    const uint32_t count = static_cast<uint32_t>(rows.size());
    std::memcpy(blob.data(), &count, sizeof(count));
    if (!rows.empty())
        std::memcpy(blob.data() + kReportBytes, rows.data(), rows.size() * sizeof(coop::net::DevParityEntry));
    return coop::blob_chunks::SendBlob(&session, coop::net::ReliableKind::DevParityReport, ++g_seq, blob);
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    if (ArmOf() == Arm::Off) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(chunk, senderSlot, blob)) return;
    uint32_t count = 0;
    if (blob.size() < kReportBytes) {
        UE_LOGW("[PILE-DEFER] a parity report of %zu bytes from slot %u is shorter than its count -- dropping",
                blob.size(), static_cast<unsigned>(senderSlot));
        return;
    }
    std::memcpy(&count, blob.data(), sizeof(count));
    if (count > MaxRows() || blob.size() != kReportBytes + static_cast<size_t>(count) * sizeof(coop::net::DevParityEntry)) {
        UE_LOGW("[PILE-DEFER] a parity report from slot %u names %u rows in %zu bytes -- dropping",
                static_cast<unsigned>(senderSlot), count, blob.size());
        return;
    }
    Report r;
    r.slot = senderSlot;
    r.rows.resize(count);
    if (count) std::memcpy(r.rows.data(), blob.data() + kReportBytes, count * sizeof(coop::net::DevParityEntry));
    g_reports.push_back(std::move(r));
}

bool TakeReport(Report& out) {
    if (g_reports.empty()) return false;
    out = std::move(g_reports.front());
    g_reports.pop_front();
    return true;
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

}  // namespace coop::dev::pile_defer_report
