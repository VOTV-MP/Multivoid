// coop/creatures/kerfur_dress_sync.h -- an Omega's outfit is the host's, and a pat is seen by every player.
//
// The outfit is the Omega's `drip` rows (ue_wrap/actors/kerfur_dress). The host's copy is the outfit: the host sends it
// whole (KerfurDressState) at every updateDrip of an Omega that has an element and, for every Omega, to a joiner once the
// NPC connect broadcast has told the joiner's copies by eid. No send follows an Omega's registration: the game dresses no
// Omega before it registers (a loaded save's updateDrip runs before any client connects, and the join covers that).
// A client writes the host's rows to its copy with updateDrip and rebuilds an open equipment list; a word for a copy not
// bound yet waits at the NPC mirror's bound seam. A client's put-on and take-off of an item ride coop/props/prop_act_omega, which asks the
// host and applies only what the host grants; nothing here writes `drip` from a player's act. A pat plays on the patter's
// copy as the game runs it, a client asks the host (KerfurDressIntent), the host shows it on its copy, and every other
// peer is sent the outfit with the pat flag; a receiver plays the montage alone. MTA: the server grants a vehicle entry
// before the client animates it and the client's notify completes it (CGame.cpp:3079-3196, 3373), and a client change the
// server does not keep is sent back to its source alone (:2779-2793); the 272 is the element-data relay to every other
// peer (:2768). Game thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {
class Session;
struct KerfurDressPayload;
struct KerfurDressPatPayload;
}  // namespace coop::net

namespace coop::kerfur_dress_sync {

namespace internal {   // pure: the dispatcher drives them
// A 272 the host's encoder could have written: count <= 8; `names` walks to exactly `count` rows of 1..26 printable ASCII,
// each ended by its NUL, inside its 216 bytes; every byte after the last NUL zero; flags only bit 0, `pad` zero. The
// dispatcher has already required the exact length of the word.
bool WellFormed(const coop::net::KerfurDressPayload& p);
void Unpack(const coop::net::KerfurDressPayload& p, std::vector<std::wstring>& out);   // the rows of a well-formed word
}  // namespace internal

// Store the session and watch the Omega's updateDrip. Idempotent; from the load-window installs.
void Install(coop::net::Session* session);

// Settles the watch (dead or live, said once). Game thread, once per pump tick.
void Tick();

// HOST: `omega`'s whole outfit, to `slot`, or to every client when `slot` is negative. False when it was not sent.
bool SendOutfit(void* omega, int slot);

// HOST: every Omega's outfit to the joiner, once its copies are told by the NPC connect broadcast.
void SendJoin(int slot);

// The seat's actionName watch: a pat on `kerfur` is about to run / has run on this machine. The seat arms at the PRE when
// the game's own guard lets the pat play, and the POST asks the host (a client) or shows every client (the host).
void OnPatPre(void* kerfur);
void OnPatPost(void* kerfur);

// CLIENT: the host's outfit for an Omega, and a pat to show.
void OnState(const coop::net::KerfurDressPayload& p);

// CLIENT: the NPC mirror of `eid` is bound (a spawned mirror or an adopted twin): the word held for it is applied.
void OnMirrorBound(uint32_t eid);

// CLIENT: the NPC mirror of `eid` ended before it was bound or after: the word held for it is dropped.
void OnMirrorEnded(uint32_t eid);

// HOST: a client's pat of an Omega.
void OnPatAsk(int senderSlot, const coop::net::KerfurDressPatPayload& p);

// The dispatcher refused a client's 273 (the wrong role, an unseated sender or a length that is not 4): counted, so a drill
// reads what the refusal cost.
void NoteIntentDropped();

// The session ended: everything is forgotten.
void OnDisconnect();

namespace dev {   // called only from src/coop/dev/ and the red sites below; compiled always, inert unless called
// The red arms of the Omega outfit drill (config row kdress_red), each withholding one effect, never a refusal. Read once.
//   Apply     the host writes no row at a put-on's commit (prop_act_omega)
//   Ask       a client's take-off PRE cancels and asks nothing (prop_act_omega)
//   Pat       the host sends no pat to a client (here)
//   Snapshot  the host sends a joiner no outfit (here)
enum class Red : uint8_t { Off, Apply, Ask, Pat, Snapshot };
Red RedOf();

// The host's judge of a client's 273, each message one count: a dispatcher drop or one outcome. `firstMs`/`lastMs` are the
// net clock at the first and the last message the judge saw since ResetPatSpan.
struct PatView {
    uint32_t dropped = 0, accepted = 0, notReady = 0, dead = 0, budget = 0, noOmega = 0, unreachable = 0, noPat = 0;
    uint64_t firstMs = 0, lastMs = 0;
};
PatView Pats();
void ResetPatSpan();
}  // namespace dev

}  // namespace coop::kerfur_dress_sync
