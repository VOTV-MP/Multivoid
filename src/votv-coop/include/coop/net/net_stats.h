// coop/net/net_stats.h -- session TRAFFIC ACCOUNTING (the one owner of the wire counters).
//
// MTA precedent: CNetworkStats (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CNetworkStats.{h,cpp}) -- the client-side net-stats HUD keeps bytes/packets totals and a
// timed rate window over them. Our split: TOTALS are counted here at Session's own GNS
// choke points (every accepted SendMessages / every packet entering HandleMessage), while
// the "right now" RATES come from GNS's real-time telemetry (m_flIn/OutBytesPerSec,
// summed across live connections by Session's existing ~1 Hz net-thread status sample) --
// wire-level truth including retransmits/acks, which no application-side window can see.
//
// One owner, no parallel counters (RULE 2): Session's old sent_/recv_ packet counters
// MOVED here; Session::packetsSent()/packetsRecv() delegate. Totals reset at Session::
// Start (a new session starts at zero) and SURVIVE disconnect (the panel keeps showing
// what the finished session moved). All entry points are lock-free atomics -- safe from
// the game thread (reliable sends), the net thread (pose stream / receive / sample), and
// the render thread (the ui/net_stats_panel reader).

#pragma once

#include <cstdint>

namespace coop::net::net_stats {

// Session::Start: a new session's totals begin at zero (rates zero too).
void ResetSession();

// One wire send accepted by GNS (counts 1 packet + `bytes` on the wire, header included).
void AddSent(uint32_t bytes);

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
