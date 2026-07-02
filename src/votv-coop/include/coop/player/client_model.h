// coop/player/client_model.h -- name-keyed body-skin assets + the ONE apply path.
//
// Goal (docs/COOP_CLIENT_MODEL.md): every player carries a body-SKIN choice (v93
// skins system). A skin name resolves to a converter pak's cooked assets:
//   mesh    /Game/Mods/VOTVCoop/<name>.kerfurOmega_KelSkin
//   texture /Game/Mods/VOTVCoop/tex_<name>.tex_<name>
// Every converter pak splices into the kerfurOmega_KelSkin template on the anthro
// kerfurOmegaV1_Skeleton, so the local AnimBP drives any skin 1:1 -- only the
// SkeletalMesh (+ a slot-0 'tex' MID) swaps; the AnimClass never changes.
// "dr_kel" (or empty) = the native stock body, no pak.
//
// Gameplay-layer POLICY (principle 7): name->asset resolution + caching + the
// canonical apply. The pak load is ue_wrap::asset_load; the component writes are
// ue_wrap::engine/puppet. Graceful degrade: an unresolvable skin leaves the body
// as-is (the peer missing a pak sees kel). Game thread only.

#pragma once

#include <string>

namespace coop::client_model {

// True for "" / "dr_kel": the stock body (no pak assets involved).
bool IsNativeSkin(const std::string& name);

// The cooked USkeletalMesh for `name`, lazily loaded from the auto-mounted pak.
// Per-name cache with GUObjectArray-slot liveness revalidation: a level-change
// GC can collect a pak asset, so a cached hit is IsLiveByIndex-checked and
// re-loaded when stale (the recycled-address trap that plain IsLive misses --
// [[feedback-islive-unsafe-on-freed-cached-pointer]]). Null when the pak/asset
// is missing; the NULL result is also cached (a missing pak is not re-probed
// per puppet spawn) but a rescue re-probe happens if the name is re-requested
// after a Refresh-driven re-apply.
void* GetSkinMesh(const std::string& name);

// The skin's atlas texture (tex_<name> package), same cache discipline. Null
// when absent -- the mesh then renders with the stock kel material (mis-mapped
// but harmless).
void* GetSkinTexture(const std::string& name);

// THE apply path -- local pawn, fresh puppet, and mid-session change all route
// here. Writes BOTH body slots (mesh_playerVisible + the inherited
// ACharacter::Mesh -- the two-body invariant,
// [[lesson-attachparent-visibility-two-body]]):
//   custom skin: SetSkeletalMesh(mesh) x2, then slot-0 MID 'tex' override x2.
//   dr_kel:      SetSkeletalMesh(nativeMesh) x2, then SetMaterial(0, null) x2
//                (clears a previous skin's MID so kel doesn't wear our atlas).
// `nativeMesh` = the pristine kel asset (local_body::NativeBodyMesh() or the
// spawn-time baseline). Returns false when nothing was applied (custom mesh
// unresolvable, or nativeMesh null on a dr_kel apply) -- caller keeps the
// current body; the miss is logged. Game thread only.
bool ApplySkinToBody(void* mainPlayerActor, const std::string& name, void* nativeMesh);

}  // namespace coop::client_model
