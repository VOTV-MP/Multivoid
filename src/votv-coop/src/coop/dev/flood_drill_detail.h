// coop/dev/flood_drill_detail.h -- what the flood drill's files share (flood_drill.cpp, _host, _client). Dev only; see
// coop/dev/flood_drill.h.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::flood_drill::detail {

inline constexpr const char* kPrefix = "fld ";
inline constexpr uint64_t kBoundMs = 180000;       // a step that waits this long for its event says INVALID and stops
inline constexpr uint64_t kWalkBoundMs = 480000;   // the client's walk to the pipe, the director's own deadline included
inline constexpr uint64_t kJoinBoundMs = 420000;   // a peer's boot, join and world load, or a rejoin's
// How far a client's level may sit from the host's at a chat line: the lane's step (flood_sync::dev::LevelEpsilon, 1 unit), which
// the host has not yet sent, plus half a unit for the host's read to the client's across the chat. The green runs' seven reads
// of the two levels differed by 0 to 0.4 unit (the rise is about half a unit a second); the margin over that is 1.1.
inline constexpr float kLevelTolUu = 1.5f;

enum class Arm : uint8_t { Off, Burst, Mend, Far, Join, MendOld };
Arm ArmOf();
const char* ArmName();

// ---- the log, the chat, the verdict and the bound ----
void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);   // the text after `prefix` of the last chat line that starts with it
bool Seen(const std::string& line);
void Invalid(const char* why);
void Fail(const char* fmt, ...);                           // said here, and told to the other peer
void Pass(const char* why);
bool Ended();
void Finish();                                             // this peer has nothing more to do
void Go();                                                 // a step began: its bound starts again
bool Late(const char* why, uint64_t boundMs = kBoundMs);   // true when the step's bound ran out (it has said INVALID)
bool PeerFailed();                                         // the other peer said it failed: this peer finishes

// ---- what a peer reads ----
struct Reading {
    std::string bits;   // every pipe's bit in object-name order
    float level = 0.f;
};
bool Read(Reading& out);                                   // false (INVALID said) when the world has no pipe or no flood volume
std::string Line(const char* head, const Reading& r);      // "<head> <bits> <level>"
bool Parse(const std::string& rest, Reading& out);
// The client's check of the host's line against its own reading: the pipes equal, and the levels within kLevelTolUu.
bool Matches(const Reading& mine, const Reading& host, std::string& why);

// ---- the connection, the tick and each peer's steps ----
bool HostMayStart(coop::net::Session& s);
bool ClientMayStart();
bool RejoinedLife();                                       // the client's second life (the rig's --rejoin-env)
void HostTick(coop::net::Session& s);
void ClientTick(coop::net::Session& s);
void ResetHost();
void ResetClient();
void ArmFirstTick();
void ResetAll();

}  // namespace coop::dev::flood_drill::detail
