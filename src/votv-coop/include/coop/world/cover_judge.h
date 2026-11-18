// coop/world/cover_judge.h -- the host's half of the cover lane (see coop/world/cover_sync.h): a client's ask judged
// on the host's own desk or station, the change run on its copy, the desk's plates and each station's door sent when
// they change and to a joiner. cover_sync.cpp calls it; nothing else does. Game thread.
#pragma once

namespace coop::net {
struct CoverPayload;
}  // namespace coop::net

namespace coop::cover_judge {

// HOST, per frame: the answers owed, the fresh plate props tended, the queued asks judged, the changes sent.
void Tick();

// HOST: a client's ask, queued for its judge.
void OnIntent(int senderSlot, const coop::net::CoverPayload& p);

// HOST, at a joiner's world-ready edge: the desk's plates and every station's door, to that slot.
void SendJoinSnapshotForSlot(int slot);

// Whether `actor` is a plate prop a take-off just granted that has not yet left its lid in a hold, and whether any is.
bool IsFresh(void* actor);
bool AnyFresh();

// Session end: the queued asks, the answers owed and the sent records are cleared.
void OnDisconnect();

}  // namespace coop::cover_judge
