// coop/interactables/notebook_sync.h -- a notebook's page text across peers.
//
// The page text is the notebook's own save record (`text`, in the stream betterGetData writes and betterLoadData reads), which
// already crosses on the prop record lane (coop/props/prop_save_data). This module adds the editor:
//   - one editor at a time: ui_notebook_C open on a notebook holds the notebook's occupancy claim;
//   - what the editor wrote reaches the record: the editing peer publishes at the editor's close and, while it
//     holds the claim, at the editor's own commit, at most once per kPublishEvery when `text` changed;
//   - the host takes a client's record from the slot holding the claim, or from any slot while nobody does;
//   - a client lets the claim go only after the host has answered its last publish, so the close reaches
//     the host while its sender still holds; a bound makes it let go anyway;
//   - a peer holding the claim applies no record for that notebook once it has published in this claim:
//     its edit is then the newer.
// The two rules are registered with coop/props/prop_record_gates; the hold on the release and the end of a claim
// with coop/interactables/device_occupancy. The page-add verb is cancelled while another peer holds the notebook.
// Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::notebook_sync {

// Store the session, watch the editor's commit, its quit and the page-add verb, and register the rules (idempotent;
// re-entered by the subsystem pump until the watches take). Both roles.
void Install(coop::net::Session* session);

// HOST: may the record a client sent for the notebook `actor` land? From the slot holding its claim, or
// from any slot when none holds it; a record from another slot is refused, logged.
bool AcceptsClientRecord(void* actor, uint8_t senderSlot);

// Session teardown: the session, the last-published text, the close in flight, the unanswered publishes.
void OnDisconnect();

// What only the notebook drill reads (coop/dev/notebook_drill); nothing else in the mod may.
namespace dev {

// What the lane has done since the session began: records the host's rule took from the slot holding the claim,
// from any slot while nobody held it, and refused; the records this peer published; and the ticks a release of the
// local claim waited for the host's answer, with how many releases ended at the 10 s bound instead of on it.
struct Stats {
    uint32_t acceptedHeld = 0;
    uint32_t acceptedFree = 0;
    uint32_t refused = 0;
    uint32_t releaseWaits = 0;
    uint32_t published = 0;
    uint32_t releasedByBound = 0;
};
Stats StatsNow();

// Has the lane taken its first look at this notebook (at the editor's first commit), the text a later edit is
// measured from? An edit made before that is not a change to publish.
bool BaselineTaken(void* notebook);

}  // namespace dev

}  // namespace coop::notebook_sync
