// coop/dev/prop_syncer_drill_detail.h -- IMPLEMENTATION-PRIVATE (src tree, not include/): what the prop syncer
// drill's roles share. prop_syncer_drill.cpp holds the host, the acting client and the helpers declared here;
// prop_syncer_drill_observer.cpp holds the observing client. Game thread only.

#pragma once

#include "coop/props/prop_drive_stream.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::prop_syncer_drill::detail {

enum class Arm : uint8_t { Off, Kick, Forge };
Arm ArmOf();

inline constexpr uint8_t  kActorSlot = 1, kObserverSlot = 2;
inline constexpr uint32_t kMinRows = 3;   // rows of the granted generation a copy takes before the End
inline constexpr float    kTravelFrac = 0.5f, kRestCm = 5.f, kRestDeg = 3.f;   // the swinger drill's settle tolerance
// Failure bounds (ms), never waits: each phase leaves on its edge.
inline constexpr uint64_t kReadyBoundMs = 180000; // host: start to both ready lines; client: to the go line
inline constexpr uint64_t kSayBoundMs = 240000;   // the go line to the client's rested line
inline constexpr uint64_t kEndBoundMs = 15000;    // a line to this peer's own End applied
inline constexpr uint64_t kGenBoundMs = 15000;    // observer (forge): the kicked line to a relayed row

// The lobby-chat lines no save can hold. Each client says its ready line once its own streams run (StreamsRun); the
// host says the go line after both.
inline constexpr const char* kClientReadyLine = "PSYNC client ready";
inline constexpr const char* kObserverReadyLine = "PSYNC observer ready";
inline constexpr const char* kGoLine = "PSYNC go";
inline constexpr const char* kKickedPrefix = "PSYNC kicked ";
inline constexpr const char* kRestedPrefix = "PSYNC rested ";
inline constexpr const char* kEndedPrefix = "PSYNC ended ";

// This client's streams run, as a player in a settled world: both clients' ready condition.
bool StreamsRun();
// The load arm: the filler the host queues to the kicking client, in KiB (0 = none). Host and client read the row.
long LoadKb();
uint64_t Now();                              // steady-clock ms
std::string Fmt(const char* f, ...);
void Pass(const char* role, bool& done, const std::string& numbers);   // the role's DONE PASS line, tagged PROP-SYNC
void Fail(const char* role, bool& done, const std::string& why);       // the role's FAIL: line, tagged PROP-SYNC

// The client's "PSYNC rested <eid> <cm> <p y r x y z>": its distance, its copy's pose at rest.
struct Rested {
    uint32_t         eid = 0;
    float            cm = 0.f;
    ue_wrap::FRotator rot{};
    ue_wrap::FVector  loc{};
};
bool ReadRested(Rested* r);
// The eid in the last live line that starts with `prefix`.
bool ReadEid(const char* prefix, uint32_t* eid);

// What this peer's copy of the prop shows against the client's pose: empty `why` when the stream took it kTravelFrac
// of the client's distance (when asked) and the copy rests within kRestCm / kRestDeg of the End's pose. The travel is
// judged at the last row applied before the End (the view's lastRowLoc), since the End places the copy at the client's
// pose and would satisfy any distance by itself.
struct Verdict {
    std::string why;
    float       travelled = 0.f, offCm = 0.f;
};
Verdict JudgeCopy(const Rested& r, const coop::prop_drive_stream::dev::DriveView& v, bool needTravel);

// The host's phases (prop_syncer_drill_host.cpp).
void HostTick(Arm a);
void HostReset();

// The observing client's phases (prop_syncer_drill_observer.cpp).
void ObserverTick(coop::net::Session* s, Arm a);
void ObserverReset();

}  // namespace coop::dev::prop_syncer_drill::detail
