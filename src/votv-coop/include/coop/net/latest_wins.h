// coop/net/latest_wins.h -- a client's changes of one single-valued state on the host, the newest kept: the lane
// applies a changed value at most at its budget, and past it holds the newest value as owed, applied once a token
// refills. An honest player's last choice always lands; a fast or hostile sender loses only the values in between,
// and costs the host no more applications than the budget. A refusal would leave the host on an older value, so
// nothing is refused. One instance per slot and kind, beside the lane's pending value in one PerSlotState so both
// clear with the occupant. A keyed state (one value per device) or a claim that owes its sender an answer is not
// this helper's: it is a verb, paced in its lane. One thread per instance, as IntentBucket.

#pragma once

#include "coop/net/intent_bucket.h"

#include <cstdint>

namespace coop::net {

// MTA relays a player's sync to the others at a zone's interval and skips what falls inside it, the next update
// carrying the state then current (CPlayer.cpp:981-1030; the intervals, CBandwidthSettings.h:98, set per preset).
// Ours diverges in four ways, each for the cost it bounds. The states are sent once per change, not as a stream, so
// the newest skipped value is kept and paid when the budget allows. The host's own copy is paced, not only the relay:
// the host's application is itself the cost (a mirror spawned, a model loaded, a feed line), so the host's view of
// the state lags the newest by at most a refill. A burst comes before the pace, so an honest quick run of changes
// lands at once. The pace is per sender, not per viewer: what is bounded is what one client makes every peer do.
// Source keeps a player's changed settings pending while its rules refuse them and applies them on a later think
// (player.cpp:3467-3473,3536,4641-4645); its flag holds no value, reading the engine's current settings when it
// applies, and waits on a rule, where ours holds the value the wire carried once and waits on a rate.
// MTA's nearest analog for a client changing a shared state is element data (CGame.cpp:2715-2796): applied at once,
// relayed to all but the source, the server's value sent back to the source when a handler cancels it. Paced here for
// the host's own cost; compared by the receiver, where MTA's sending client compares (its client-side
// CStaticFunctionDefinitions.cpp:1032) and its server applies every arrival; an owed sender is not re-told, so a
// crossing leaves apart only the one past its budget.

class LatestWins {
public:
    // A changed value at `nowMs`, a steady clock's milliseconds, which the lane has stored as its pending one: true
    // when a token pays for it now (apply it; nothing stays owed), else the value is owed.
    bool Offer(const IntentBudget& budget, uint64_t nowMs) {
        owed_ = !bucket_.Take(budget, nowMs);
        return !owed_;
    }

    // The payment, asked each tick: true when a value is owed and a token pays for it now (apply the pending value).
    bool Due(const IntentBudget& budget, uint64_t nowMs) {
        if (!owed_ || !bucket_.Take(budget, nowMs)) return false;
        owed_ = false;
        return true;
    }

    // The newest value equals the applied one: nothing is owed.
    void Withdraw() { owed_ = false; }

    bool Owed() const { return owed_; }

    // The arithmetic: a first value at once, a debt past the burst, paid only at a refill and only when owed, a
    // withdrawal, and a value-initialised instance owing nothing. Logs ALL PASS or each failure.
    static bool RunSelftest();

private:
    IntentBucket bucket_;
    bool         owed_ = false;
};

}  // namespace coop::net
