// coop/dev/late_join_drill_detail.h -- what the late-join drill's two files share: the host's staging entry point and
// the name of the staged drive's row, which the client's scan and verdict read too. Impl-private: nothing
// outside coop/dev includes it. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::late_join_drill::detail {

// The row name the host writes into the staged drive. A save's own drives carry other names, so a drive with this
// one on a client is the staged drive and no other.
inline constexpr const wchar_t* kDriveName = L"LATE-JOIN-DRIVE";

// What the host writes into the other staged subjects, which the client's verdict reads back: the Kerfus's energy
// (not its default), the staged rack's row count, and the prefix of its rows' names.
inline constexpr float kKerfusEnergy = 37.f;
inline constexpr int kRackRows = 2;
inline constexpr const wchar_t* kRackRowPrefix = L"LATE-JOIN-RACK-";

// HOST: one tick of the staging leg (late_join_drill_host.cpp).
void HostTick(coop::net::Session& session);

}  // namespace coop::dev::late_join_drill::detail
