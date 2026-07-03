#include "coop/player/client_model.h"

#include "coop/player/skin_effects.h"
#include "coop/player/skin_registry.h"
#include "ue_wrap/asset_load.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"

#include <cstdint>
#include <map>

namespace coop::client_model {

namespace {

// Per-name asset cache. `tried` remembers a load MISS so a missing pak is not
// re-probed on every puppet spawn; a HIT is revalidated by GUObjectArray slot
// (IsLiveByIndex) because a level-change GC can collect a pak asset and recycle
// its address -- a stale hit re-loads instead of dereferencing an impostor.
struct CachedAsset {
    void*   ptr = nullptr;
    int32_t idx = -1;
    bool    tried = false;
};

std::map<std::string, CachedAsset> g_meshCache;
std::map<std::string, CachedAsset> g_texCache;

std::wstring Widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());  // names are validated ASCII
}

void* ResolveCached(std::map<std::string, CachedAsset>& cache, const std::string& name,
                    const std::wstring& path, const char* what) {
    namespace R = ue_wrap::reflection;
    CachedAsset& c = cache[name];
    if (c.ptr && R::IsLiveByIndex(c.ptr, c.idx)) return c.ptr;
    if (c.ptr) {
        UE_LOGI("client_model: cached %s for skin '%s' was GC'd -- re-loading", what, name.c_str());
        c.ptr = nullptr;
        c.tried = false;  // the asset existed before; re-probe
    }
    if (c.tried) return nullptr;  // known-missing pak: stay silent per spawn
    c.tried = true;
    c.ptr = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
    if (c.ptr) {
        c.idx = R::InternalIndexOf(c.ptr);
        UE_LOGI("client_model: skin '%s' %s ready (%p)", name.c_str(), what, c.ptr);
    } else {
        UE_LOGW("client_model: skin '%s' %s NOT loadable (pak absent on this machine?) -- "
                "native kel fallback", name.c_str(), what);
    }
    return c.ptr;
}

}  // namespace

bool IsNativeSkin(const std::string& name) {
    return name.empty() || name == coop::skins::kNativeSkinName;
}

void* GetSkinMesh(const std::string& name) {
    if (IsNativeSkin(name) || !coop::skins::IsValidSkinName(name)) return nullptr;
    // v94 builtin kerfur skins: the game's own kerfurOmegaV1_Skeleton bodies,
    // loaded by asset path (no pak; materials ship with the mesh).
    if (const wchar_t* builtin = coop::skins::BuiltinSkinPath(name))
        return ResolveCached(g_meshCache, name, builtin, "mesh");
    const std::wstring w = Widen(name);
    // Package <name>, object kerfurOmega_KelSkin: the converter splices every
    // model into the kel-skin template and does not rename the export
    // (docs/COOP_CLIENT_MODEL.md 6a); the package path disambiguates from the
    // game's own kerfurOmega_KelSkin.
    return ResolveCached(g_meshCache, name,
                         L"/Game/Mods/VOTVCoop/" + w + L".kerfurOmega_KelSkin", "mesh");
}

void* GetSkinTexture(const std::string& name) {
    if (IsNativeSkin(name) || !coop::skins::IsValidSkinName(name)) return nullptr;
    if (coop::skins::BuiltinSkinPath(name)) return nullptr;  // builtins carry their own materials
    const std::wstring w = Widen(name);
    return ResolveCached(g_texCache, name,
                         L"/Game/Mods/VOTVCoop/tex_" + w + L".tex_" + w, "texture");
}

bool ApplySkinToBody(void* mainPlayerActor, const std::string& name, void* nativeMesh) {
    namespace E = ue_wrap::engine;
    namespace Pup = ue_wrap::puppet;
    if (!mainPlayerActor) return false;

    void* comps[2] = { Pup::GetMeshPlayerVisibleComponent(mainPlayerActor),
                       Pup::GetNativeBodyMeshComponent(mainPlayerActor) };

    if (IsNativeSkin(name)) {
        if (!nativeMesh) {
            UE_LOGW("client_model: dr_kel apply on %p but no native mesh captured yet -- skipped",
                    mainPlayerActor);
            return false;
        }
        int done = 0;
        for (void* comp : comps) {
            if (!comp) continue;
            if (E::SetSkeletalMesh(comp, nativeMesh)) ++done;
            // Clear a previous skin's MID override so kel renders its own material.
            E::SetComponentMaterial(comp, 0, nullptr);
        }
        UE_LOGI("client_model: %p -> native dr_kel body (%d/2 slots, material override cleared)",
                mainPlayerActor, done);
        // Tear down a previous builtin skin's effect rig (face actor / light /
        // particles) -- dr_kel wears none.
        if (done > 0) coop::skin_effects::Apply(mainPlayerActor, name);
        return done > 0;
    }

    void* mesh = GetSkinMesh(name);
    if (!mesh) return false;  // pak missing here -- keep the current body (logged in resolver)
    int done = 0;
    for (void* comp : comps) {
        if (!comp) continue;
        if (E::SetSkeletalMesh(comp, mesh)) ++done;
    }
    // Slot-0 MID 'tex' override on both slots (inst_kel4_body is a MIC of
    // mat_object_sk whose diffuse is the 'tex' param -- no cooked material needed).
    // No texture (v94 builtin kerfur skins carry their OWN materials) -> CLEAR the
    // slot-0 override instead, or a previous pak skin's MID atlas stays painted
    // over the builtin's material (the dr_kel revert lesson applied here).
    int bound = 0;
    void* tex = GetSkinTexture(name);
    for (void* comp : comps) {
        if (!comp) continue;
        if (tex) {
            void* mid = E::CreateDynamicMaterialInstance(comp, 0);
            if (mid && E::SetTextureParameterValue(mid, L"tex", tex)) ++bound;
        } else {
            E::SetComponentMaterial(comp, 0, nullptr);
        }
    }
    UE_LOGI("client_model: %p -> skin '%s' (mesh %d/2 slots, %s)",
            mainPlayerActor, name.c_str(), done,
            tex ? "atlas tex bound" : "own materials (override cleared)");
    // The skin is more than the mesh for builtin kerfur skins: rebuild the
    // variant's effect rig (RT face / alive glow / particles / step FX) --
    // AFTER the material override pass above so the rig's slot writes land on
    // the final material state. Non-builtins tear a previous rig down.
    if (done > 0) coop::skin_effects::Apply(mainPlayerActor, name);
    return done > 0;
}

}  // namespace coop::client_model
