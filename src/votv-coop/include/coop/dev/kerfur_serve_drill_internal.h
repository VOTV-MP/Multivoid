// coop/dev/kerfur_serve_drill_internal.h -- what the Omega serve drill (legs A to C) shares with its leg D
// (kerfur_serve_drill_disc.cpp). Not for any other file.

#pragma once

#include "ue_wrap/core/script_gate.h"

#include <string>

namespace coop::dev::kerfur_serve_drill::detail {

// A proper disc: the insert refuses the white one.
inline constexpr const wchar_t* kDisc = L"prop_floppyDisc_R_C";

// The drill's two ways to end a peer on a step that could not run: each says INVALID on the rig verdict line.
// `tag` is the drill's name on its `rig: VERDICT` line.
void ClientInvalid(const char* why, const char* tag);
void HostInvalid(const char* why, const char* tag);

// The actor the player's hand holds (holding_actor), or null. Game thread.
void* HoldingActor(void* player);

// The Omega mirror nearest the host's body, within the spawn's reach of it; null when none. Game thread.
void* FindMirrorByHost();

// The `name` argument of a watched actionName call, false when it did not read. Game thread.
bool VerbNameOf(const ue_wrap::script_gate::Call& c, std::wstring& out);

}  // namespace coop::dev::kerfur_serve_drill::detail
