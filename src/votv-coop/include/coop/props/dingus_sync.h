// coop/props/dingus_sync.h -- the dingus (Maxwell) found once for every player.
//
// A dingus plays its music until it is found. The find is `touch`, which runs on the finder's machine alone (a grab, or an
// action), and a copy born elsewhere from a drop takes no record, the dingus being a food the food lane owns.
//
// Each peer keeps a set of the keys found, at most kMaxFound. The host's find (the script gate sees `touch` end on a dingus
// by name) adds the key and says DingusFound (280) to every client; a client's find is asked of the host, DingusReport (281),
// which judges the sender and the key and, if it accepts, makes the find its own. Every copy under a key of the set is marked
// found as `loadData` does, with no achievement: a copy alive when the key is heard, a copy the host births (a drop, a
// load) when its birth is drained, a copy a client binds when it binds. A joiner gets the set inside its snapshot.

// Gameplay and network layer (principle 7); engine access through ue_wrap/actors/dingus. Game thread, but the birth
// callback, which any thread may run.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct DingusFoundPayload;
struct DingusReportPayload;
struct WireKey;
}  // namespace coop::net

namespace coop::dingus_sync {

// The keys one peer holds: a world holds a handful of placed dingi and the store sells none. MTA's element data has no count cap;
// here a set a client's reports feed is bounded.
inline constexpr size_t kMaxFound = 64;

// From the session's first tick (subsystems::InstallLoadWatchers), before the host's census seed: the session, the watch on
// `touch`, the birth subscription, the class table and the world-change reset. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Each session tick: the class table, the watch's settle, and on the host the births queued since the last tick. The tick that
// finds the four classes after one lacked reads every live copy once (a copy born while its class was unknown was passed over).
// Returns at once when nothing is queued. Game thread.
void Tick();

// A client's copy was bound to an element id (remote_prop's bind line): marked found when its key is in the set. Every role.
void OnBound(void* actor, const std::wstring& key);

// HOST->ALL: a dingus is found. On a client; the dispatcher has refused the wrong way, the wrong length and a malformed key.
void OnFound(const coop::net::DingusFoundPayload& p);

// CLIENT->HOST: a client's report, judged (dingus_judge.cpp). The dispatcher has refused the wrong way, the wrong length and a
// malformed key.
void OnReport(int senderSlot, const coop::net::DingusReportPayload& p);

// A message the dispatcher refused for its direction, length or key: counted here and said at the first and each power of two.
void CountDropped(int senderSlot, size_t payloadLen);

// Whether a key off the wire is one this lane sends: 1..31 printable ASCII characters (0x21..0x7E). The dispatcher asks it.
bool WireKeyWellFormed(const coop::net::WireKey& key);

// HOST: every key of the set to a joiner, before its snapshot.
void SendJoinFound(int slot);

void OnDisconnect();

namespace dev {

// What the host's judge has counted since the session began (the dingus drill and the hostile arm read it): the reports that
// reached it, the accepted ones, the refusals by reason in the order notReady, dead, budget, known, noCopy, notDingus,
// unreachable, and the messages the dispatcher refused. `firstMs` and `lastMs` are the net clock at the first and the latest report
// judged since ResetJudgeSpan (0 when none): the token bucket refills with time, so the arm that counts refusals reads the span.
inline constexpr int kJudgeReasons = 7;
struct JudgeView {
    uint32_t judged = 0, accepted = 0, refused[kJudgeReasons] = {}, dropped = 0;
    uint64_t firstMs = 0, lastMs = 0;
};
JudgeView Judge();
void ResetJudgeSpan();

// Whether this peer's found set holds `key`, and how many keys it holds.
bool FoundHas(const std::wstring& key);
size_t FoundKeyCount();

// The drill's reads at the lane's own edges, each called with the copy and its key after the lane's work on it: `born` after
// the host drained a dingus's birth, `bound` after a client's bind line. One pointer compare when none is set. Game thread.
struct Observer {
    void (*born)(void* actor, const std::wstring& key);
    void (*bound)(void* actor, const std::wstring& key);
};
void SetObserver(const Observer* observer);

}  // namespace dev

}  // namespace coop::dingus_sync
