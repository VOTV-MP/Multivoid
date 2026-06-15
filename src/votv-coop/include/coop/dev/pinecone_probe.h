// coop/dev/pinecone_probe.h -- dev-only verification probe for the pinecone-scare
// coop sync (ini pinecone_probe=1). RULE-1 "verify, don't guess": the pinecone
// RE concluded the scare ALREADY mirrors host->client through the generic
// Aprop_C Init-POST -> PropSpawn pipeline (the user's "doesn't sync" premise was
// a misread of a CLIENT-side branch). Before writing any sync code, PROVE it:
// this probe force-spawns a prop_food_pinecone_C on the HOST exactly the way
// pineconeSpawner does (BeginDeferred + FinishSpawningActor -> the prop's Init
// fires our host Init-POST observer), so we can read both logs and confirm the
// host broadcasts + the client mirrors. Fires ONCE a few seconds after a client
// is connected. Dev-only, host-only; never ships (RULE 3).

#pragma once

namespace coop::dev::pinecone_probe {

// Idempotent install (resolves the spawn refs + class; latches). Call per tick.
void Install();

// Host-only, fires the single force-spawn once ~5 s after isConnected. No-op on
// the client / when the ini gate is off / after it has fired.
void Tick(bool isConnected, bool isHost);

}  // namespace coop::dev::pinecone_probe
