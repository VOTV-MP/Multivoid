// coop/player/client_model.h -- the remote CLIENT puppet's custom body mesh.
//
// Goal (docs/COOP_CLIENT_MODEL.md): remote CLIENT puppets render a custom
// character model (a cooked USkeletalMesh shipped in scientist.pak) while the
// HOST puppet stays Dr. Kel. The custom mesh rides the SAME
// kerfurOmegaV1_Skeleton as the stock kel skin, so the local anthro AnimBP
// drives it 1:1 -- only the SkeletalMesh swaps; the AnimClass stays the local
// one. Role gate lives at the puppet spawn site (net_pump): slot 0 == host
// (kel), slots >= 1 == clients (custom mesh).
//
// Gameplay-layer POLICY (principle 7): decides WHICH mesh a client puppet wears
// + caches the loaded asset. The actual pak load is ue_wrap::asset_load. Graceful
// degrade: nullptr when the pak is absent -> the puppet keeps the kel skin.
// Game thread only.

#pragma once

namespace coop::client_model {

// The custom client-puppet USkeletalMesh, lazily loaded on first call from the
// auto-mounted pak, then cached for the process. Returns nullptr if the
// pak/asset is missing (the caller keeps the default kel skin). One-shot: a null
// result is remembered so a missing pak is not re-probed on every puppet spawn.
void* GetClientPuppetMesh();

}  // namespace coop::client_model
