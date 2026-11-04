// coop/world/vent_crawler_sync.h -- the vent crawler's crawl, as a client hears and sees it.
//
// The crawler (ventCrawler_C, the day 5 event) is the host's: a client has its npc mirror, whose BeginPlay and box
// overlap are refused, and the crawl's pose, the doors' moves and the vent's swing reach it on their own lanes. What
// the crawl plays from its own graph does not: the two vent steps, the steps along the crawl, the metal break and its
// burst, and the signal room door's power, which the crawl writes as a field. The host watches the crawler's box
// overlap, whose first call starts the crawl, and sends one EventCue; the client plays the rest on the crawl's own
// schedule: the steps 2 and 2.5 s after it, the timeline 5 s after it, its events at the times and with the volume
// and pitch its own tracks give, read from the mirror's timeline. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::vent_crawler_sync {

// The EventCue id of a crawl's start; the cue registry's ids are append-only.
inline constexpr uint32_t kCueCrawl = 102;

// The host's watch on the box overlap, registered once and retried until its name resolves.
void Install(coop::net::Session* session);

// CLIENT: the crawl's sounds, burst and door power as their times come.
void Tick();

// CLIENT: the host's cue. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// The crawl's sounds this client has played, for the drill.
unsigned SoundsPlayed();

void OnDisconnect();

}  // namespace coop::vent_crawler_sync
