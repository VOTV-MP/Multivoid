// coop/props/snack_act.h -- a client's use of a world snack, through the act lane (coop/props/prop_act_*): the commit's
// words for a snack, what the client reads of its copy around the replay, and the host's bounded apply of what the
// client reports. Gameplay layer (principle 7). Game thread unless marked.
//
// The snack's eater is the client's own save (addFood, the beer's nausea, the milk's irradiation), so the host cannot
// run the use for it: the client replays the game's own actionOptionIndex on its copy and reports what changed, and the
// host applies that delta to ITS copy within a bound (an opened bottle only closed-to-open, one use only from a copy that
// has one and is open or not a beer).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::snack_act {

// The commit's `committed` byte for a snack: 0 aborted, 1 the client's copy ended (its last use), 2 the eater ran and the
// copy lives. `flags` is required on every snack commit: bit 0 a use was spent, bit 1 the bottle was opened.
inline constexpr uint8_t kCommitApplied = 2;
inline constexpr uint8_t kFlagSpent = 1;
inline constexpr uint8_t kFlagOpened = 2;
inline constexpr uint8_t kFlagMask = kFlagSpent | kFlagOpened;

// The state of a client's copy, read just before and just after its replay.
struct CopyState {
    bool read = false;
    int32_t uses = 0;
    bool open = false;
};
CopyState ReadCopy(void* actor);

struct CommitWords {
    uint8_t committed = 0;
    uint8_t flags = 0;
};
// The client's commit for a replay that `ran` (the verb could be called) and whether its copy `destroyed` inside it:
// the copy ended (1 with the spent bit), the eater ran and the copy lives (2 with what changed), or neither (an abort,
// 0 with no flags). A full eater reads no change and sends 2 with no flags.
CommitWords CommitFor(bool ran, bool destroyed, const CopyState& before, const CopyState& after);

namespace internal {   // pure, no engine call: prop_act_selftest drives these (NOT the game thread)
// Is a snack commit well formed: committed 1 carries exactly the spent bit, 2 carries no bit outside the two. (An abort
// is judged apart, and a non-snack's commit never carries a bit or a 2.)
bool WellFormedSnackCommit(uint8_t committed, uint8_t flags);
// The host's bounded apply: what its copy holds after the client's report, from what it holds now. `opened` takes a
// closed beer to open and nothing else; `spent` takes one use only from a copy that has one and is open or not a beer.
struct Bounded {
    int32_t uses;
    bool open;
};
Bounded Bound(int32_t uses, bool open, bool beer, uint8_t flags);
}  // namespace internal

// HOST: a client's matched commit on a snack whose reservation stood. Applies the delta to `actor` within the bound
// (internal::Bound), through the snack's record, and publishes the snack's state to every peer. A copy that reaches no
// use left is destroyed on every peer. `committed` is 1 or 2, `flags` well formed (the judge checked both).
void OnCommit(coop::net::Session* s, void* actor, const coop::net::PropActCommitPayload& p, uint8_t senderSlot);

// CLIENT: while alive, the wrapper that the replayed snack's script births (a FinishSpawningActor whose source is that
// snack) is destroyed at once and never published: the host births the one wrapper. Armed for one replay's latch; inert
// for a null snack or before the hook has installed.
class WrapperRefusal {
public:
    explicit WrapperRefusal(void* snack);
    ~WrapperRefusal();
    WrapperRefusal(const WrapperRefusal&) = delete;
    WrapperRefusal& operator=(const WrapperRefusal&) = delete;

private:
    bool armed_;
};

// HOST: a snack's use the client aborted (its replay did not run, or left its copy unread, and may have fed the eater):
// the host's state is marked for publication so the client's copy is put back.
void NoteAborted(void* actor);

// HOST: the host's own action 4 on `actor` ran (the POST of the two rows). A world snack (an element of the host's own,
// not a hand view) is marked for publication; anything else is ignored.
void NoteHostUse(void* actor);

// Per pump tick: on the host, the one drain that publishes each marked snack's record to every peer (a mark of an actor
// that died or is no longer a world snack is dropped), and the settle of the spill hook (the beer's grime birth marks
// its beer). A client has nothing to drain.
void Tick(coop::net::Session* s);

namespace dev {   // the act drill's snack phase
// Marks a snack the drill wrote through its record on the host, so the drain publishes it. Game thread.
void MarkForPublication(void* actor);
// Whether a mark still waits for the drain: the host's publication for it is not sent yet. Game thread.
bool MarksPending();
}  // namespace dev

// The session ended: the marks are dropped.
void OnDisconnect();

}  // namespace coop::snack_act
