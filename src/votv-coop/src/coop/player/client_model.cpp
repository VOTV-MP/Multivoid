#include "coop/player/client_model.h"

#include "ue_wrap/asset_load.h"
#include "ue_wrap/log.h"

namespace coop::client_model {

namespace {
// Package `scientist` at mount root /Game/Mods/VOTVCoop/ (auto-mounted from
// Content/Paks/LogicMods/votv-coop/scientist.pak). The OBJECT name is still
// `kerfurOmega_KelSkin` -- the cook spliced the scientist into the kel-skin
// template and did not rename the export (docs/COOP_CLIENT_MODEL.md 6a). The
// full package.object path disambiguates from the game's OWN kerfurOmega_KelSkin
// (a different package: /Game/meshes/kerfurAnthro/sk/...).
constexpr const wchar_t* kClientMeshPath =
    L"/Game/Mods/VOTVCoop/scientist.kerfurOmega_KelSkin";

void* g_mesh = nullptr;
bool  g_tried = false;
}  // namespace

void* GetClientPuppetMesh() {
    if (g_tried) return g_mesh;  // cache incl. a null failure: don't re-probe a missing pak per spawn
    g_tried = true;
    g_mesh = ue_wrap::asset_load::LoadObjectByPath(kClientMeshPath);
    if (g_mesh)
        UE_LOGI("client_model: custom client puppet mesh ready (%p) -- client puppets wear it", g_mesh);
    else
        UE_LOGW("client_model: no custom client mesh (pak absent or load failed) -- client "
                "puppets keep the default kel skin");
    return g_mesh;
}

}  // namespace coop::client_model
