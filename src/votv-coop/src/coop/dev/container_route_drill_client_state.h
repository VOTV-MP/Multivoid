// coop/dev/container_route_drill_client_state.h -- what the client half of the route census drill shares between
// container_route_drill_client.cpp (the session steps) and container_route_drill_client_hand.cpp (the hand route R4).
// A private header of coop/dev/container_route_drill_client*.cpp (src tree, not include/).

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>

namespace coop::dev::container_route_drill::detail {

inline constexpr uint64_t kHostBoundMs = 60000;   // the host's raw write, its round trip and the host's chat

enum class Step : uint8_t {
    Arm, Find, Settle, Open, OpenWait, R1, R1Wait, R1Done, R2Refill, R2, R2Call, R2Wait, R3, R3Use, R3Wait,
    R4Close, R4, R4HoldWait, R4Settle, R4Armed, R4Aim, R4Wait,
    ViewOpen, ViewOpenWait, ViewWait, BeforeReady, BeforeWait, BeforeTaken, BeforeTake, BeforeSettle, DoneWait, Done
};

// The fixtures: the backpack, and the loose drive R4 holds and throws.
extern ue_wrap::CachedObjRef g_pack, g_drive;

// The step machine: enter a step, the time since it was entered, a route step that failed (the FAIL line, and the drill
// goes on to `next`), the census after a route's step and on, and the actor in the player's hand.
void Next(Step s);
bool Expired(uint64_t bound);
void RouteFailTo(const char* phase, Step next);
void Finish(const char* label, Step next);
void* HeldActor(void* player);

// The hand route R4 (the hand file): each is one step of ClientTick's switch; HandReset starts it over.
void StepR4Close();
void StepR4(void* player);
void StepR4HoldWait();
void StepR4Settle(void* player);
void StepR4Armed();
void StepR4Aim(void* player);
void StepR4Wait();
void HandReset();

}  // namespace coop::dev::container_route_drill::detail
