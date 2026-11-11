// coop/net/net_stats.h -- session TRAFFIC ACCOUNTING (the one owner of the wire counters).
//
// MTA precedent: CNetworkStats (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CNetworkStats.{h,cpp}) -- a client-side net-stats HUD keeping bytes and packet totals with a
// timed rate window over them. Our split: TOTALS are counted here at Session's own GNS choke
// points (every accepted SendMessages, every packet entering HandleMessage), while the "right
// now" RATES come from GNS's real-time telemetry (m_flIn/OutBytesPerSec, summed across live
// connections by Session's ~1 Hz net-thread status sample) -- wire-level truth including
// retransmits and acks, which no application-side window can see.
//
// ONE owner of the SESSION totals: packetsSent()/packetsRecv() delegate here; the per-link counters
// in coop/net/send_rate_control.h are a different quantity (reliable bytes, per slot, reset per
// connection), never a copy of these. Totals reset at Session::Start and SURVIVE disconnect, so the
// panel keeps showing what the finished session moved. Every entry point is a lock-free atomic, so
// the game thread, the net thread and the render thread (ui/net_stats_panel) are all safe.

#pragma once

#include <cstdint>

namespace coop::net::net_stats {

// Session::Start: a new session's totals begin at zero (rates zero too).
void ResetSession();

// One wire send accepted by GNS (counts 1 packet + `bytes` on the wire, header included).
void AddSent(uint32_t bytes);

// One reliable message of `kind` (a ReliableKind value) accepted for at least one peer by Session::SendReliable or
// SendReliableToSlot: the two single-message choke points, so a batch (a blob's chunks) is not counted here. A dev drill
// reads it as a before/after pair around a span; nothing in the shipping path reads it.
void AddReliableSent(uint16_t kind);
uint32_t ReliableSent(uint16_t kind);

// One wire packet entering Session::HandleMessage (full datagram length).
void AddRecv(uint32_t bytes);

// Session's ~1 Hz net-thread status sample publishes the GNS real-time view:
// bytes/s + packets/s summed across live connections, the live peer count, the worst
// ping among them (-1 = none measured), and whether the transport is connected.
// Disconnected -> all zeros + connected=false (totals stay).
void PublishRates(float inBps, float outBps, float inPktps, float outPktps,
                  int peers, int pingMaxMs, bool connected);

uint64_t PacketsSent();
uint64_t PacketsRecv();

struct Snapshot {
    uint64_t bytesSent = 0;    // session total, wire bytes accepted by GNS
    uint64_t bytesRecv = 0;    // session total, wire bytes delivered to HandleMessage
    uint64_t packetsSent = 0;
    uint64_t packetsRecv = 0;
    float inBps = 0.f;         // GNS real-time, summed across live conns (~1 s window)
    float outBps = 0.f;
    float inPktps = 0.f;
    float outPktps = 0.f;
    int peers = 0;             // live connections (client: 1 = the host link)
    int pingMaxMs = -1;        // worst live ping, -1 = none measured
    bool connected = false;
};
void Get(Snapshot& out);

}  // namespace coop::net::net_stats
