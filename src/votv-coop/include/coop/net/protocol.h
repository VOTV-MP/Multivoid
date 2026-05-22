// coop/net/protocol.h -- the wire format (Phase 3, serialization layer).
//
// Methodology 3.3: this sits ABOVE the pure-I/O transport and BELOW the session
// application layer. It is just POD structs + (de)serialization; it knows nothing
// about sockets and nothing about the engine. Layout is fixed, packed, and
// little-endian (LAN-first: both peers are x86-64 Windows, so we send the structs
// raw -- no endian swap needed; the magic+version guard a future mismatch).
//
// Phase 3.4 "position-only first": the only state packet is a PoseSnapshot
// (x,y,z,yaw,speed). Input/equipment/entity packets (Phase 4) get new MsgTypes.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::net {

// Opaque magic guard (rejects stray datagrams that hit our port). Both peers
// just agree on the constant; the spelling is "VMTP" (VoTv MultiPlayer).
inline constexpr uint32_t kMagic = 0x564D5450u;
inline constexpr uint16_t kProtocolVersion = 1;

// Default LAN port (overridable via votv-coop.ini "net.port=").
inline constexpr uint16_t kDefaultPort = 47621;

enum class MsgType : uint8_t {
    Hello = 1,         // handshake (either direction); no payload beyond the header
    PoseSnapshot = 2,  // a player pose (unreliable: freely dropped, newest wins)
    Bye = 3,           // graceful disconnect; no payload
    Reliable = 4,      // a reliable, ordered, ack'd message (chat / system events)
    ReliableAck = 5,   // acknowledges a Reliable message by its relSeq
};

// Payload kinds carried inside a Reliable message. The chat/event-feed groundwork:
// Join announces a player (its nickname) so the peer can post "<nick> joined" and
// label the remote player. (Chat text is a future ReliableKind.)
enum class ReliableKind : uint8_t {
    Join = 1,  // payload: [uint8 len][len bytes UTF-8 nickname]
};

#pragma pack(push, 1)

// Every datagram starts with this. seq is per-sender, monotonically increasing
// (ordering + stale-drop on the receiver; never trust an older seq than the last).
// token is the session nonce: the host mints a random one at session start and
// hands it out in its Hello; thereafter EVERY packet must carry it or it is
// dropped. An off-path spoofer never sees the token, so it cannot inject
// pose/Bye into an established session (anti-hijack / anti-replay). A client's
// FIRST Hello (before it has learned the token) carries 0.
struct PacketHeader {
    uint32_t magic;    // kMagic
    uint16_t version;  // kProtocolVersion
    uint8_t  type;     // MsgType
    uint8_t  _pad;     // reserved
    uint32_t seq;      // per-sender sequence number
    uint64_t token;    // session nonce (0 == "not yet known", client's first Hello)
};
static_assert(sizeof(PacketHeader) == 20, "PacketHeader must be 20 bytes");

// Position-only pose (methodology 3.4). Floats are UE4 cm / degrees (UE4.27's
// FVector/FRotator are float, not double). yaw is the horizontal facing; speed is
// the horizontal velocity magnitude (cm/s) -> the remote AnimBP locomotion blend.
struct PoseSnapshot {
    float x, y, z;
    float yaw;
    float speed;
};
static_assert(sizeof(PoseSnapshot) == 20, "PoseSnapshot must be 20 bytes");

struct PosePacket {
    PacketHeader header;
    PoseSnapshot pose;
};
static_assert(sizeof(PosePacket) == 40, "PosePacket must be 40 bytes");

// A reliable message: standard header (type=Reliable, seq=the reliable seq) +
// ReliableHeader + variable UTF-8 payload. relSeq is a SEPARATE counter from the
// header seq space (the unreliable pose seq); it lives in the header's seq field
// for reliable packets and is what the ack references.
struct ReliableHeader {
    uint8_t  kind;     // ReliableKind
    uint8_t  _pad[3];
    uint16_t payloadLen;
    uint16_t _pad2;
};
static_assert(sizeof(ReliableHeader) == 8, "ReliableHeader must be 8 bytes");

#pragma pack(pop)

// Largest datagram we ever send/receive. Recv buffers size to this.
inline constexpr int kMaxPacketBytes = 256;

// Max reliable payload that fits one datagram: 256 - 20 (PacketHeader) - 8 (ReliableHeader).
inline constexpr int kMaxReliablePayload = kMaxPacketBytes - 20 - 8;

// Coordinate / speed sanity bounds (cm). A pose outside these is garbage or a
// hostile teleport-spam and is REJECTED at the trust boundary so non-finite or
// absurd values never reach the engine transform (SetActorLocation). VOTV's map
// is a few km; +/-1e6 cm (10 km) is generous headroom.
inline constexpr float kMaxCoord = 1.0e6f;
inline constexpr float kMaxSpeed = 1.0e5f;  // cm/s (well above any real walk/sprint)

// Fill a header in-place.
inline void WriteHeader(PacketHeader& h, MsgType type, uint32_t seq, uint64_t token) {
    h.magic = kMagic;
    h.version = kProtocolVersion;
    h.type = static_cast<uint8_t>(type);
    h._pad = 0;
    h.seq = seq;
    h.token = token;
}

// Validate a received buffer as one of ours: enough bytes + magic + version.
// Returns the parsed header fields and true if the header is well-formed.
inline bool ParseHeader(const void* data, int len, MsgType& outType, uint32_t& outSeq,
                        uint64_t& outToken) {
    if (len < static_cast<int>(sizeof(PacketHeader))) return false;
    PacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kProtocolVersion) return false;
    outType = static_cast<MsgType>(h.type);
    outSeq = h.seq;
    outToken = h.token;
    return true;
}

// Reject a pose that is non-finite (NaN/Inf) or outside sane world bounds, BEFORE
// it can reach the engine. true == safe to apply.
inline bool ValidatePose(const PoseSnapshot& p) {
    const float vals[5] = {p.x, p.y, p.z, p.yaw, p.speed};
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    if (std::fabs(p.x) > kMaxCoord || std::fabs(p.y) > kMaxCoord || std::fabs(p.z) > kMaxCoord)
        return false;
    if (p.speed < 0.f || p.speed > kMaxSpeed) return false;
    return true;
}

}  // namespace coop::net
