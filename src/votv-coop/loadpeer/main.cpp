// loadpeer/main.cpp -- the load peer (target multivoid-loadpeer, dev only, never shipped): one
// process that joins a host as a client and holds a seat with no game behind it, linked against
// coop-net alone. It does the real admission, answers AssignPeerSlot with one Join (the encoder
// coop/net/join_wire.h shares with the DLL), says ClientWorldReady, publishes a pose at 60 Hz on a
// small circle around the host's own position, and counts what arrives by reliable kind: every
// inbound message is counted and dropped. No voice. One session per process (the session and the
// identity are process-wide), so N peers are N processes; the identity is the temporary one an exe
// outside a `dlls` folder gets.

#include "coop/build_trust/build_trust.h"
#include "coop/net/join_wire.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/version.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kUsage =
    "multivoid-loadpeer: joins a Multivoid host, holds a seat and sends poses; no game needed.\n"
    "\n"
    "usage: multivoid-loadpeer --host <ip:port> --game-build-key <key> --build-sha <64 hex>\n"
    "                          [--index <n>] [--seconds <s>] [--world-ready <0|1>]\n"
    "\n"
    "  --host <ip:port>        the host's address (the port defaults to the game's when omitted)\n"
    "  --game-build-key <key>  the host's game build key, as its log prints it (game_build)\n"
    "  --build-sha <64 hex>    the build claim's hash: the host's main.dll sha256 (flags 0)\n"
    "  --index <n>             element id 32768+n and nick load-<n> (default 0)\n"
    "  --seconds <s>           how long to hold the seat once seated (default 60)\n"
    "  --world-ready <0|1>     send ClientWorldReady after the Join (default 1)\n"
    "  --help                  this text\n"
    "\n"
    "exit: 0 after a clean stop; 1 when the link ended or the seat was not taken; 2 on a bad argument.\n";

// The wait for a seat is bounded: a dial that nothing answers is reported, not held forever.
constexpr int kSeatWaitSeconds = 30;
constexpr int kPoseIntervalMs = 16;
constexpr int kPoseLagRebaseMs = 100;
constexpr uint32_t kEidBase = 32768;  // the Join's element id lies in [32768, 65536)

struct Args {
    std::string ip;
    int port = coop::net::kDefaultPort;
    std::string key;
    std::string shaHex;
    int index = 0;
    int seconds = 60;
    bool worldReady = true;
};

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseSha(const std::string& hex, uint8_t* out) {
    if (hex.size() != 2 * coop::build_trust::kShaBytes) return false;
    for (size_t i = 0; i < coop::build_trust::kShaBytes; ++i) {
        const int hi = HexDigit(hex[2 * i]);
        const int lo = HexDigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

// The whole of `s` as a number in [lo, hi]; anything else (empty, a trailing letter, a sign where none
// fits, out of range) is refused.
bool ParseInt(const std::string& s, int lo, int hi, int* out) {
    int v = 0;
    const char* end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (s.empty() || r.ec != std::errc() || r.ptr != end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

bool BadValue(const std::string& k, const std::string& v) {
    std::fprintf(stderr, "loadpeer: %s '%s' is not an accepted value (--help)\n", k.c_str(), v.c_str());
    return false;
}

// "<ip>" or "<ip>:<port>", the port 1..65535; an IPv6 literal is not taken.
bool ParseHost(const std::string& s, Args& a) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos) {
        a.ip = s;
        return !s.empty();
    }
    if (s.find(':', colon + 1) != std::string::npos) return false;
    a.ip = s.substr(0, colon);
    return !a.ip.empty() && ParseInt(s.substr(colon + 1), 1, 65535, &a.port);
}

// 0 = run, 1 = --help was asked, 2 = a bad argument (already said).
int ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--help" || k == "-h") return 1;
        if (i + 1 >= argc) {
            std::fprintf(stderr, "loadpeer: %s needs a value\n", k.c_str());
            return 2;
        }
        const std::string v = argv[++i];
        bool ok = true;
        if (k == "--host") ok = ParseHost(v, a);
        else if (k == "--game-build-key") a.key = v;
        else if (k == "--build-sha") a.shaHex = v;
        else if (k == "--index") ok = ParseInt(v, 0, 32767, &a.index);
        else if (k == "--seconds") ok = ParseInt(v, 0, 86400, &a.seconds);
        else if (k == "--world-ready") {
            ok = v == "0" || v == "1";
            a.worldReady = v == "1";
        } else {
            std::fprintf(stderr, "loadpeer: unknown argument %s\n", k.c_str());
            return 2;
        }
        if (!ok) {
            BadValue(k, v);
            return 2;
        }
    }
    uint8_t sha[coop::build_trust::kShaBytes];
    if (a.ip.empty() || a.key.empty() || !ParseSha(a.shaHex, sha)) {
        std::fprintf(stderr, "loadpeer: --host, --game-build-key and a 64-hex --build-sha are required "
                             "(--help)\n");
        return 2;
    }
    return 0;
}

bool SendJoin(coop::net::Session& session, const Args& a) {
    coop::net::JoinFields f;
    f.eid = kEidBase + static_cast<uint32_t>(a.index);
    f.nick = "load-" + std::to_string(a.index);
    f.prefsFlags = 1;
    f.game = coop::version::kGameTarget;
    ParseSha(a.shaHex, f.buildSha);
    f.buildFlags = 0;
    f.gameBuild = a.key;
    const std::vector<uint8_t> payload = coop::net::EncodeJoin(f);
    return session.SendReliableToSlot(0, coop::net::ReliableKind::Join, payload.data(),
                                      static_cast<int>(payload.size()));
}

// The host's pose is a peer's bytes: the circle is built on it only when it is finite and well inside the
// world's bounds, so no value we send is one the receive boundary would refuse.
bool CentreUsable(const coop::net::PoseSnapshot& c) {
    const float limit = coop::net::kMaxCoord - 1000.f;
    for (const float v : {c.x, c.y, c.z})
        if (!std::isfinite(v) || std::fabs(v) > limit) return false;
    return true;
}

// A standing player on a 3 m circle (cm) around `c`, walking its tangent. Every value finite and
// in the pose's range.
coop::net::PoseSnapshot CirclePose(const coop::net::PoseSnapshot& c, double t, int index) {
    constexpr double kRadiusCm = 300.0;
    constexpr double kRadPerSec = 0.5;
    const double a = t * kRadPerSec + index * 0.7;
    coop::net::PoseSnapshot p{};
    p.x = c.x + static_cast<float>(kRadiusCm * std::cos(a));
    p.y = c.y + static_cast<float>(kRadiusCm * std::sin(a));
    p.z = c.z;
    double yaw = (a + 1.5707963267948966) * 57.29577951308232;  // the tangent, in degrees
    yaw = std::fmod(yaw, 360.0);
    if (yaw > 180.0) yaw -= 360.0;
    p.yaw = static_cast<float>(yaw);
    p.speed = static_cast<float>(kRadiusCm * kRadPerSec);
    p.stateBits = 0;  // standing: not in air, not ragdolled, not burning
    p.healthFrac = 255;
    p.foodFrac = 255;
    p.sleepFrac = 255;
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    const int parsed = ParseArgs(argc, argv, a);
    if (parsed == 1) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (parsed == 2) return 2;

    coop::net::Session session;
    coop::net::Config cfg;
    cfg.role = coop::net::Role::Client;
    cfg.selfAddressed = true;
    cfg.peerIp = a.ip;
    cfg.port = static_cast<uint16_t>(a.port);
    coop::net::Refusal why{};
    if (!session.Start(cfg, {}, &why)) {
        std::printf("loadpeer %d: could not start (%s)\n", a.index, coop::net::Describe(why.code).id);
        return 1;
    }

    std::map<uint16_t, uint32_t> byKind;
    uint32_t reliables = 0;
    uint64_t posesSent = 0;
    int seat = -1;
    bool joinSent = false;
    bool worldReadySent = false;
    bool ended = false;
    std::string endText;
    coop::net::PoseSnapshot centre{};
    bool haveCentre = false;

    const auto begin = Clock::now();
    Clock::time_point seatedAt{};
    Clock::time_point nextPose = begin;
    while (true) {
        const auto now = Clock::now();
        coop::net::Session::ReliableMessage msg;
        while (session.TryGetReliable(msg)) {
            ++reliables;
            ++byKind[static_cast<uint16_t>(msg.kind)];
            if (msg.kind == coop::net::ReliableKind::AssignPeerSlot && seat < 0 &&
                msg.payloadLen >= sizeof(coop::net::AssignPeerSlotPayload)) {
                seat = msg.payload[0];
            }
        }
        if (seat >= 0 && !joinSent) joinSent = SendJoin(session, a);
        if (joinSent && a.worldReady && !worldReadySent)
            worldReadySent = session.SendReliableToSlot(0, coop::net::ReliableKind::ClientWorldReady, nullptr, 0);
        if (joinSent && seatedAt == Clock::time_point{}) seatedAt = now;

        const coop::net::HostClose close = session.TakeHostCloseReason();
        if (close.code != coop::net::EndReason::None || !close.text.empty()) {
            ended = true;
            endText = std::string(coop::net::Describe(close.code).id) + " " + close.text;
            break;
        }
        if (seatedAt == Clock::time_point{}) {
            if (now - begin > std::chrono::seconds(kSeatWaitSeconds)) {
                ended = true;
                endText = "no seat within " + std::to_string(kSeatWaitSeconds) + " s";
                break;
            }
        } else if (now - seatedAt >= std::chrono::seconds(a.seconds)) {
            break;
        }

        // The circle is centred on the host's own pose, the first finite one that arrives; no pose goes out
        // before it.
        if (!haveCentre) {
            haveCentre = session.TryGetRemotePose(0, centre) && CentreUsable(centre);
        } else if (now >= nextPose) {
            const double t = std::chrono::duration<double>(now - begin).count();
            session.SetLocalPose(CirclePose(centre, t, a.index));
            ++posesSent;
            // A stall (a long drain, a descheduled process) does not turn into a burst of catch-up poses.
            if (now - nextPose > std::chrono::milliseconds(kPoseLagRebaseMs)) nextPose = now;
            nextPose += std::chrono::milliseconds(kPoseIntervalMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    session.Stop();

    std::string kinds = "{";
    for (const auto& kv : byKind) {
        if (kinds.size() > 1) kinds += ", ";
        kinds += std::to_string(kv.first) + ":" + std::to_string(kv.second);
    }
    kinds += "}";
    const bool clean = !ended && joinSent && (worldReadySent || !a.worldReady) && posesSent > 0;
    if (clean) {
        std::printf("loadpeer %d: seated slot %d, %s, %u reliables by kind %s, %llu poses sent, exit clean\n",
                    a.index, seat, a.worldReady ? "world-ready" : "no world-ready", reliables, kinds.c_str(),
                    static_cast<unsigned long long>(posesSent));
        return 0;
    }
    const char* verdict = "the seat was not completed";
    if (ended) verdict = endText.c_str();
    else if (joinSent && posesSent == 0) verdict = "no usable host pose arrived";
    std::printf("loadpeer %d: %s, %u reliables by kind %s, %llu poses sent, exit NOT clean (%s)\n", a.index,
                seat >= 0 ? ("seated slot " + std::to_string(seat)).c_str() : "never seated", reliables,
                kinds.c_str(), static_cast<unsigned long long>(posesSent), verdict);
    return 1;
}
