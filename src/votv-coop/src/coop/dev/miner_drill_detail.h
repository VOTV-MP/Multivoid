// coop/dev/miner_drill_detail.h -- what the mining rig drill's files share (miner_drill.cpp, _rig, _use, _claim, _record,
// _join). Dev only; see coop/dev/miner_drill.h.

#pragma once

#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::miner_drill::detail {

enum class Arm { Off, Use, Claim, Record, Join };

// The step bound: a step that waits this many ticks for its event says INVALID and ends. It never decides a PASS or a FAIL.
inline constexpr int kBoundTicks = 3600;
// The use arm's flood: this many asks in one tick, of which the host's bucket (4 at once) grants its burst.
inline constexpr int kFloodAsks = 12;
inline constexpr int kBurst = 4;
// The slots the host gives the use arm's rig, so every GPU granted finds room (16 is the game's own limit).
inline constexpr int kSlots = 16;
// The points the host writes into the claim arm's rig.
inline constexpr int kClaimPoints = 50;
// A rig further than this from the client's body would be refused as out of reach before the drill's own question.
inline constexpr float kReachCm = 500.f;

// ---- miner_drill.cpp: the arm, the log, the chat, the verdict ----
Arm ArmOf();
const char* TagOf(Arm arm);
extern int g_stepTicks;
extern ue_wrap::CachedObjRef g_rig;   // the arm's rig: the host's own, or the client's copy of it
extern uint32_t g_eid;                // its element id

void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Seen(const std::string& line);
bool Rest(const std::string& prefix, std::string* out);   // the text after `prefix` of the last line that starts with it
bool Late(const char* why);   // true when the step's bound ran out (the step has said INVALID)
void Invalid(const char* why);   // the host says INVALID and ends the drill; a client tells the host, which says it
void Verdict(coop::rig_verdict::Outcome outcome, const char* fmt, ...);   // the HOST's verdict line; ends the drill
void Go(int& step, int next);

// ---- miner_drill_rig.cpp: reads, spawn and the game's own use ----
struct Held {
    ue_wrap::reflection::FName name{};
    std::wstring               text;
    void*                      actor = nullptr;
};
int   EidOf(void* actor);
int32_t Balance();                                  // this peer's credit balance, -1 unread
bool  ReadHeld(void* player, Held& h);              // the player's holding_name and holding_actor
bool  CallUse(void* rig, void* player, const Held& h);   // the game's use of the held item on the rig, a blocking hit
// A rig before the host's player and `rightCm` to its right, dropped from the player's height.
void* SpawnRig(void* me, float fwdCm, float rightCm = 0.f);
void* LiveProp(uint32_t eid);                       // the live prop an element id names, or null
bool  RigQuiet(void* rig);                          // true after it has been at rest for a run of ticks
bool  ClientMayStart();                             // this client's join is over
bool  HostMayStart(coop::net::Session& s);          // slot 1's join is over
float PuppetDistance(void* rig);                    // from slot 1's body to the rig, -1 when either is unread
// The loose GPUs (prop_gpu_C) within kNearCm of `body`, each as its slot and serial, so a GPU that appears later is told from
// the ones that were there and from a destroyed one's successor in the same slot. NewGpusNear counts those not in `before`.
inline constexpr float kNearCm = 400.f;
std::vector<uint64_t> GpusNear(void* body);
int NewGpusNear(void* body, const std::vector<uint64_t>& before);
// The integers of `text` separated by spaces, up to `max`: how many were read.
int Numbers(const std::string& text, long* out, int max);
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
// The five numbers and the powered flag of `rig` as one text, for the log.
std::string RigText(void* rig);

// ---- the arms; each file keeps its own steps ----
void UseHost(coop::net::Session& s);
void UseClient(coop::net::Session& s);
void UseReset();
void ClaimHost(coop::net::Session& s);
void ClaimClient(coop::net::Session& s);
void ClaimReset();
void RecordHost(coop::net::Session& s);
void RecordClient(coop::net::Session& s);
void RecordReset();
void JoinHost(coop::net::Session& s);
void JoinClient(coop::net::Session& s);
void JoinReset();

}  // namespace coop::dev::miner_drill::detail
