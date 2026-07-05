// coop/net/net_stats.cpp -- see coop/net/net_stats.h.

#include "coop/net/net_stats.h"

#include <atomic>

namespace coop::net::net_stats {
namespace {

std::atomic<uint64_t> g_bytesSent{0};
std::atomic<uint64_t> g_bytesRecv{0};
std::atomic<uint64_t> g_packetsSent{0};
std::atomic<uint64_t> g_packetsRecv{0};
std::atomic<float>    g_inBps{0.f};
std::atomic<float>    g_outBps{0.f};
std::atomic<float>    g_inPktps{0.f};
std::atomic<float>    g_outPktps{0.f};
std::atomic<int>      g_peers{0};
std::atomic<int>      g_pingMaxMs{-1};
std::atomic<bool>     g_connected{false};

}  // namespace

void ResetSession() {
    g_bytesSent.store(0, std::memory_order_relaxed);
    g_bytesRecv.store(0, std::memory_order_relaxed);
    g_packetsSent.store(0, std::memory_order_relaxed);
    g_packetsRecv.store(0, std::memory_order_relaxed);
    PublishRates(0.f, 0.f, 0.f, 0.f, 0, -1, false);
}

void AddSent(uint32_t bytes) {
    g_bytesSent.fetch_add(bytes, std::memory_order_relaxed);
    g_packetsSent.fetch_add(1, std::memory_order_relaxed);
}

void AddRecv(uint32_t bytes) {
    g_bytesRecv.fetch_add(bytes, std::memory_order_relaxed);
    g_packetsRecv.fetch_add(1, std::memory_order_relaxed);
}

void PublishRates(float inBps, float outBps, float inPktps, float outPktps,
                  int peers, int pingMaxMs, bool connected) {
    g_inBps.store(inBps, std::memory_order_relaxed);
    g_outBps.store(outBps, std::memory_order_relaxed);
    g_inPktps.store(inPktps, std::memory_order_relaxed);
    g_outPktps.store(outPktps, std::memory_order_relaxed);
    g_peers.store(peers, std::memory_order_relaxed);
    g_pingMaxMs.store(pingMaxMs, std::memory_order_relaxed);
    g_connected.store(connected, std::memory_order_relaxed);
}

uint64_t PacketsSent() { return g_packetsSent.load(std::memory_order_relaxed); }
uint64_t PacketsRecv() { return g_packetsRecv.load(std::memory_order_relaxed); }

void Get(Snapshot& out) {
    out.bytesSent   = g_bytesSent.load(std::memory_order_relaxed);
    out.bytesRecv   = g_bytesRecv.load(std::memory_order_relaxed);
    out.packetsSent = g_packetsSent.load(std::memory_order_relaxed);
    out.packetsRecv = g_packetsRecv.load(std::memory_order_relaxed);
    out.inBps       = g_inBps.load(std::memory_order_relaxed);
    out.outBps      = g_outBps.load(std::memory_order_relaxed);
    out.inPktps     = g_inPktps.load(std::memory_order_relaxed);
    out.outPktps    = g_outPktps.load(std::memory_order_relaxed);
    out.peers       = g_peers.load(std::memory_order_relaxed);
    out.pingMaxMs   = g_pingMaxMs.load(std::memory_order_relaxed);
    out.connected   = g_connected.load(std::memory_order_relaxed);
}

}  // namespace coop::net::net_stats
