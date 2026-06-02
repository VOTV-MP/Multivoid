// coop/trash_collect_sync.h -- replicate the trash-pile COLLECT interaction.
//
// VOTV: pressing E on a trashBitsPile_C ("trash stack") runs its BP
// playerTryToCollect(Player), which spawns ONE Aprop_C trash item AND
// auto-grabs it into the player's hands the SAME frame (PHC grab mode),
// repeatable until the pile's amountA/amountB are exhausted (then it
// self-destructs).
//
// The held item never mirrored because it is born with Key == None (the
// Aprop_C BP UCS hasn't minted a NewGuid yet at the instant it's grabbed),
// so our held-prop pose stream emitted PropPose key='None' (unmatchable on
// the peer) AND the Init-POST broadcast skipped it (None-key guard). Net:
// the collector held an item the other peer never saw.
//
// Fix (this module): a POST observer on trashBitsPile_C::playerTryToCollect
// reads the freshly-grabbed item, FORCE-mints a stable synth Key on it (the
// item's setKey accepts it exactly as the clump/chip classes do), and
// broadcasts a PropSpawn under that Key. From there the EXISTING held-prop
// pose stream (net_pump grabbing_actor path) carries the item into the
// collector's hands on every peer, and PropRelease / PropDestroy unwind it
// like any other held prop. No new wire message; reuses the proven path.
//
// The collected items are stable Aprop_C props (cans/bottles/etc.), NOT the
// transient self-morphing chipPile/clump -- so driving them on the peer is
// safe (the chipPile drive UAF, [[project-bug-trash-chippile-uaf-crash]], does
// not apply here).
//
// Authority (first increment): each peer collects its own local pile copy and
// broadcasts the item it made, so the held item mirrors regardless of who
// collects. The shared pile's amountA/B COUNT is not yet host-authoritative
// (two peers collecting the SAME pile can diverge) -- a follow-up; MTA
// CPickup.cpp is the host-authoritative-count shape when we get there.

#pragma once

namespace coop::net { class Session; }

namespace coop::trash_collect_sync {

// Store the live session pointer (atomic; the POST observer can fire on a
// parallel-anim worker). Mirrors garbage_sync::SetSession.
void SetSession(coop::net::Session* session);

// Install the trashBitsPile_C::playerTryToCollect POST observer. Idempotent;
// retries internally until the BP class + UFunction resolve (they load lazily
// on first world encounter -- e.g. the player nearing a trash area). Called
// per-tick from net_pump::InstallObservers (the same idempotent-retry site as
// garbage_sync), so the latch fires whenever trashBitsPile_C loads in-session.
void Install();

}  // namespace coop::trash_collect_sync
