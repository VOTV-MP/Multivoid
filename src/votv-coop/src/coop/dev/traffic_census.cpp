// coop/dev/traffic_census.cpp -- see coop/dev/traffic_census.h. NET-THREAD only; no locks.

#include "coop/dev/traffic_census.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_stats.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::traffic_census {
namespace {

constexpr int kStreamTypes = 256;        // the whole uint8_t MsgType space
constexpr int kReliableKinds = 1 << 16;  // the whole uint16_t ReliableKind space: no kind is past the table
constexpr uint64_t kFlushMs = 10000;
constexpr int kRowsPerLine = 10;

struct Row {
    uint32_t packets = 0;
    uint64_t bytes = 0;
    uint32_t loggedPackets = 0;   // the packets at the last flush: a row that did not move is not logged again
};

int g_enabled = -1;
Row g_stream[kStreamTypes], g_reliable[kReliableKinds];
uint64_t g_firstTick = 0, g_lastFlushTick = 0;
uint64_t g_totalBytes = 0, g_totalPackets = 0;

void Note(Row& r, uint32_t bytes) {
    if (!g_firstTick) g_firstTick = g_lastFlushTick = GetTickCount64();
    ++r.packets;
    r.bytes += bytes;
    ++g_totalPackets;
    g_totalBytes += bytes;
}

void FlushTable(Row* rows, int count, char tag, unsigned sinceS) {
    std::string line;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        Row& r = rows[i];
        if (r.packets == r.loggedPackets) continue;
        r.loggedPackets = r.packets;
        char cell[64];
        std::snprintf(cell, sizeof(cell), " %c%d=%u/%llu", tag, i, r.packets, static_cast<unsigned long long>(r.bytes));
        line += cell;
        if (++n == kRowsPerLine) {
            UE_LOGI("[TRAFFIC] t=%us%s", sinceS, line.c_str());
            line.clear();
            n = 0;
        }
    }
    if (n) UE_LOGI("[TRAFFIC] t=%us%s", sinceS, line.c_str());
}

}  // namespace

bool Enabled() {
    if (g_enabled < 0) g_enabled = coop::config::ResolveFlag(::coop::config_registry::rows::traffic_census) ? 1 : 0;
    return g_enabled == 1;
}

void NoteStream(unsigned msgType, uint32_t bytes) {
    if (msgType < kStreamTypes) Note(g_stream[msgType], bytes);
}

void NoteReliable(unsigned kind, uint32_t bytes) {
    if (kind < kReliableKinds) Note(g_reliable[kind], bytes);
}

void Tick() {
    if (!g_firstTick) return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastFlushTick < kFlushMs) return;
    g_lastFlushTick = now;
    const unsigned sinceS = static_cast<unsigned>((now - g_firstTick) / 1000);
    UE_LOGI("[TRAFFIC] t=%us TOTAL=%llu/%llu", sinceS, static_cast<unsigned long long>(g_totalPackets),
            static_cast<unsigned long long>(g_totalBytes));
    // The transport's own view of the wire (packets as sent, with its headers, acks and retransmits), a one-second window.
    coop::net::net_stats::Snapshot w;
    coop::net::net_stats::Get(w);
    UE_LOGI("[TRAFFIC] t=%us WIRE in=%.0f/%.1f out=%.0f/%.1f", sinceS, w.inBps, w.inPktps, w.outBps, w.outPktps);
    FlushTable(g_stream, kStreamTypes, 'S', sinceS);
    FlushTable(g_reliable, kReliableKinds, 'R', sinceS);
}

}  // namespace coop::dev::traffic_census
