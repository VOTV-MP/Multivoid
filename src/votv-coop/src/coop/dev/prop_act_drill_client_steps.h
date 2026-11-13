// coop/dev/prop_act_drill_client_steps.h -- the act drill's client step machine's state and step-end words, shared by
// prop_act_drill_client.cpp (the machine) and prop_act_drill_client_steps.cpp (the steps that wait on the host's word
// after the act). Private to the act drill.

#pragma once

#include "coop/dev/prop_act_drill.h"

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace coop::dev::prop_act_drill {

enum class CStep : uint8_t { Arm, AwaitReady, Resolve, Fill, Walk, AwaitGather, ClearHand, Grab, AwaitDriven,
                             AwaitReply, AwaitContents, AwaitPoison, AwaitCopy, AwaitUpright, Say, Done };

constexpr uint64_t kCopyBoundMs = 30000;   // a snack arm: the act's end to this client's copies showing the host's state
constexpr uint64_t kStandBoundMs = 60000;  // a step's end to the player standing again (a bound only says INVALID)

extern CStep g_cstep;                                   // the machine's step
extern uint64_t g_cNextMs;                              // the next poll of the step that waits
extern uint64_t g_nextSayMs;                            // the next chat line this client may say
extern size_t g_crow;                                   // the row the machine is on
extern coop::element::ElementId g_ceid;                 // the step's target's eid
extern ue_wrap::CachedObjRef g_mirror;                  // the step's target
extern std::string g_readyRest;                         // the text after the host's ready line's eid
extern std::unordered_set<coop::element::ElementId> g_mirrorsBefore;   // an unpacked row: the prop mirrors held before the act
extern size_t g_freshContents;                          // the unpacked row's contents that arrived
extern int g_poisonBefore;                              // foodPoison entries live before the poisoned content's bite

void CEnter(CStep s);
bool CExpired(uint64_t bound);
void ClientFail(const detail::Row& row, const std::string& why);
void ClientInvalid(const detail::Row& row, const std::string& why);
void ClientPass(const detail::Row& row, const char* what);

// The steps (prop_act_drill_client_steps.cpp), one call per poll from the machine's switch.
void StepAwaitContents(const detail::Row& row, void* player, uint64_t now);
void StepAwaitPoison(const detail::Row& row, void* player, uint64_t now);
void StepAwaitCopy(const detail::Row& row, void* player, uint64_t now);
// After a step's verdict went out and another row follows: waits for mainPlayer.isRagdoll to clear, then says the line the host
// waits on before it places the next row, and moves to it.
void StepAwaitUpright(const detail::Row& row, void* player, uint64_t now);

}  // namespace coop::dev::prop_act_drill
