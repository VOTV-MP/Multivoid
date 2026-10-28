
// coop/world/fuse_tower_rows.h -- the fuse towers' state as rows (FuseTowerState, op 0), the coordinate towers'
// and the radio tower's. The host reads its towers each tick and sends every tower's row whenever one or an
// acknowledgement changed, and all of them to a joiner at its world-ready. A client never rolls a tower
// (coop/world/fuse_tower_ops refuses it), so the rows are its towers' only author: it keeps the host's last row of
// each tower and applies it when that tower resolves, as the tower's own graph makes it (what differs by kind is
// coop/world/fuse_tower_kind's), with its own untaken fuse claims on top. The element-data shape: a client applies
// the server's value and hands its handlers the old one with the new
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientEntity.cpp:470-499). Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::fuse_tower_rows {

void Install(coop::net::Session* session);

// HOST: the rows when a tower or an acknowledgement changed, and the joiners owed theirs. CLIENT: the kept rows of
// towers that have not resolved (the towers looked up again once a second), and each panel brought to the host's.
void Tick();

// The receive edge of the host's rows (router: event_dispatch_world.cpp): a row no tower sends is dropped, with
// one warning per kind of fault: a kind that is neither tower, more lights than a puzzle has, more fuses than a
// row holds, panel flags or an id on the radio tower's row, a fuse byte past blown.
void AdmitRows(coop::net::FuseTowerPayload& p);

// CLIENT: the host's rows, admitted.
void OnRows(const coop::net::FuseTowerPayload& p);

// HOST: every tower's row to a joiner at its world-ready, or as soon as the towers resolve.
void QueueConnectBroadcastForSlot(int slot);

// HOST: an op was taken and its acknowledgement changed. The rows go to every peer now, as the taken op changed a
// tower for all of them. A refusal changes none, and is answered by HostAnswerOwed.
void HostSendNow();

// HOST: a refusal's answer is owed to its sender alone (the rows hold no news for the others). It goes at the next
// tick: one answer for the refusals that carry no claim, and one more for each refused claim, each carrying that
// claim's seq (coop/world/fuse_tower_ops FillAcks); one that could not be sent stays owed, until its slot's occupant
// changes.
void HostAnswerOwed(uint8_t slot);

// CLIENT: the rows are being run on this client's towers; a use then is theirs, not a player's.
bool Applying();

// [dev] the tower drill's reads, on a client: the host's last rows (false before any), and the changes it applied
// to each kind of tower as the tower's own graph makes them.
bool LastRows(coop::net::FuseTowerPayload& out);
struct Edges {
    uint32_t breaks = 0, repairs = 0, presses = 0, pulls = 0, inserts = 0, levers = 0, fails = 0, panels = 0;
};
Edges AppliedEdges(uint8_t kind);

// [dev] CLIENT: the flags of the first row the tower (kind, id) took, which is what a joiner's tower read on its
// arrival; false while no row has gone onto it. A new object of that tower starts over.
bool FirstAppliedFlags(uint8_t kind, int32_t id, uint8_t& flags);

// CLIENT: the kept row of the tower (kind, id) goes onto it again, as the tower's first and so silently, at the next
// tick: the tower generated its puzzle or its fuses afresh (the generators are watched in fuse_tower_ops).
void ReapplyKept(uint8_t kind, int32_t id);

void OnDisconnect();

}  // namespace coop::fuse_tower_rows
