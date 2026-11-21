// coop/session/boot_selftests.cpp -- see coop/session/boot_selftests.h.

#include "coop/session/boot_selftests.h"

#include "coop/player/nickname_arbiter.h"
#include "coop/props/prop_drive_rules.h"
#include "coop/session/join_beacon.h"
#include "coop/session/player_handshake.h"
#include "coop/text/novelty_ledger.h"

namespace coop::boot_selftests {

void RunOnce() {
    // The join beacon's Draining cases, the nickname arbiter's suffix and collision rows (a suffix
    // displacing stem characters at the cap, a variant colliding with a name another player holds:
    // several peers with chosen names to stage), the nickname's trims, and the novelty cap's
    // diverse-alphabet peer (its first draft passed by construction above a budget of 32), and the
    // drive's turn measure on a prop lying on its side: none of them is staged by a LAN drill.
    static const bool kJoinDrainingOk = coop::join_beacon::RunDrainingSelftest();
    static const bool kNickArbiterOk = coop::nickname_arbiter::RunNicknameArbiterSelftest();
    static const bool kNickSanitizerOk = coop::player_handshake::RunNicknameSanitizerSelftest();
    static const bool kNoveltyOk = coop::text::RunNoveltyLedgerSelftest();
    static const bool kTurnDegOk = coop::prop_drive_rules::RunTurnDegSelftest();
    (void)kJoinDrainingOk;
    (void)kNickArbiterOk;
    (void)kNickSanitizerOk;
    (void)kNoveltyOk;
    (void)kTurnDegOk;
}

}  // namespace coop::boot_selftests
