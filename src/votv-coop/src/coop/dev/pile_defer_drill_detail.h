// coop/dev/pile_defer_drill_detail.h -- IMPLEMENTATION-PRIVATE (src tree, not include/): what the pile deferral
// drill's two files share. pile_defer_drill.cpp holds the client's counts and window; pile_defer_drill_host.cpp holds
// the host's judge. Game thread only.

#pragma once

#include "coop/dev/pile_defer_report.h"
#include "coop/net/protocol.h"

#include <vector>

namespace coop::dev::pile_defer_drill::detail {

// A pile at rest on the host must match the joiner's within these.
inline constexpr float kPlaceCm = 2.f, kTurnDeg = 2.f;
// Above this speed (cm/s) a host pile is moving: listed, not judged.
inline constexpr float kMovingCmS = 1.f;

// Every live host-allocated trash element (a pile or a clump) this peer holds as a wire mirror or, on the host, as
// its own, with its form and pose. `mirrorsOnly` keeps the joiner's bound mirrors alone.
void CollectTrash(std::vector<coop::net::DevParityEntry>& out, bool mirrorsOnly);

// HOST: judge one finished report against the host's own trash and print the verdict.
void JudgeReport(const coop::dev::pile_defer_report::Report& report);

}  // namespace coop::dev::pile_defer_drill::detail
