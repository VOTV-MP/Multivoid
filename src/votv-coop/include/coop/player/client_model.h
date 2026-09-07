// coop/player/client_model.h -- name-keyed body-skin assets and the ONE apply path.
//
// Every player carries a body-SKIN choice (docs/players.md). A skin name resolves to a converter
// pak's cooked assets:
//   mesh    /Game/Mods/VOTVCoop/<name>.kerfurOmega_KelSkin
//   texture /Game/Mods/VOTVCoop/tex_<name>.tex_<name>
// Every converter pak splices into the kerfurOmega_KelSkin template on the anthro
// kerfurOmegaV1_Skeleton, so the local AnimBP drives any skin one-to-one: only the SkeletalMesh,
// and a slot-0 'tex' MID, swap, and the AnimClass never changes. "dr_kel", or empty, is the native
// stock body with no pak.

#pragma once

#include <string>

namespace coop::client_model {

// True for "" / "dr_kel": the stock body (no pak assets involved).
bool IsNativeSkin(const std::string& name);

// The cooked USkeletalMesh for `name`, lazily loaded from the auto-mounted pak. Per-name cache with
// GUObjectArray-slot liveness revalidation: a level-change GC can collect a pak asset, so a cached
// hit is IsLiveByIndex-checked and re-loaded when stale -- a plain IsLive would miss the
// recycled-address case, since it dereferences the maybe-freed object itself. Null when the pak or
// asset is missing, and the NULL result is cached too, so a missing pak is not re-probed per puppet
// spawn; a rescue re-probe happens if the name is requested again after a Refresh-driven re-apply.
void* GetSkinMesh(const std::string& name);

// The skin's atlas texture (tex_<name> package), same cache discipline. Null
// when absent -- the mesh then renders with the stock kel material (mis-mapped
// but harmless).
void* GetSkinTexture(const std::string& name);

// CAN THIS BODY ACTUALLY WEAR THAT SKIN -- the ONE predicate, and it is exactly what
// `ApplySkinToBody` requires. It exists because two callers were asking a WEAKER question
// (`GetSkinMesh != nullptr`) than the apply enforces: a pak carrying a mesh but no
// `tex_<name>` passed the gate, was persisted and announced, and then failed to apply on the
// very next tick -- two contradictory chat lines and two wire announces in consecutive
// frames. A gate that does not ask what the operation asks is not a gate.
//
// `Unknown` is a THIRD value on purpose and never means "bad": the resolver returns null
// WITHOUT asking inside its retry window, so treating a throttled miss as an absence would
// hide an installed skin from the picker until the player found the Refresh button. A caller
// deciding whether to REFUSE must act on `No` alone. Game thread (it may load).
enum class Wearable : uint8_t { Unknown, Yes, No };
Wearable CanWearSkin(const std::string& name);

// THE apply path -- local pawn, fresh puppet and mid-session change all route here. Writes BOTH
// body slots, mesh_playerVisible and the inherited ACharacter::Mesh, which is the two-body
// invariant:
//   custom skin: SetSkeletalMesh(mesh) twice, then a slot-0 MID 'tex' override twice.
//   dr_kel:      SetSkeletalMesh(nativeMesh) twice, then SetMaterial(0, null) twice, clearing a
//                previous skin's MID so kel does not wear our atlas.
// `nativeMesh` is the pristine kel asset (local_body::NativeBodyMesh(), or the spawn-time
// baseline). Returns false when nothing was applied -- a custom mesh that would not resolve, or a
// null nativeMesh on a dr_kel apply -- and the caller keeps the current body; the miss is logged.
// Game thread only.
bool ApplySkinToBody(void* mainPlayerActor, const std::string& name, void* nativeMesh);

}  // namespace coop::client_model
