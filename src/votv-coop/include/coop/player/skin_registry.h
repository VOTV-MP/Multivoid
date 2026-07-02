// coop/player/skin_registry.h -- the installed body-skin catalog (F1 browser source).
//
// Three skin sources, one namespace:
//   - "dr_kel" (entry 0): the native stock body, no asset load (pristine-mesh revert).
//   - BUILTIN skins (v94, user 2026-07-02 "добавь скин керфура робота"): the game's
//     own kerfurOmegaV1_Skeleton bodies (the anthro robot kerfur + its skin variants),
//     loaded by their game asset path -- no pak needed, materials come with the mesh.
//   - Converter paks in <game>/VotV/Content/Paks/LogicMods/votv-coop/: the pak
//     filename stem IS the skin name AND the package name the runtime loads
//     (/Game/Mods/VOTVCoop/<name>.kerfurOmega_KelSkin -- every pak from
//     tools/client_model/ splices into that template object).
// A sibling <name>.png or <name>.bmp in the pak dir is the browser preview tile
// (user convention 2026-07-02: previews live NEXT to the paks; works for builtin
// names too -- drop a kerfur_omega.png there).
//
// Filesystem only -- no UObject access, no network. The UI (render thread) is
// the only caller of Entries(); keep it that way (single-caller discipline, no
// lock). Name validation is shared with the wire layer (player_handshake) and
// the ini read (harness config): a skin name is a LoadObject path component
// and a reliable-payload field, so it is validated at every boundary with the
// same rule (the inventory-GUID discipline).

#pragma once

#include <string>
#include <vector>

namespace coop::skins {

// The native (pak-less) skin: Dr. Kel, the stock player body.
inline constexpr const char* kNativeSkinName = "dr_kel";

// The factory default for a NEW player identity when NONE of the curated starter
// paks (PickRandomStarterSkin) is installed. Written to votv-coop.ini player_skin=
// on first launch, right where player_guid= is generated.
inline constexpr const char* kDefaultSkinName = "hl_einstein_v1sc";

// v95 random starter (user 2026-07-02: "для НОВЫХ пиров случайный скин из списка"):
// pick a random skin for a NEW player identity from the curated converter-skin list,
// filtered to the paks actually PRESENT in LogicMods/votv-coop on this install (a
// pick that cannot load would pin the new player to the native fallback body).
// Returns kDefaultSkinName when none of the list is installed. Boot thread (config
// read path) -- touches the filesystem once.
std::string PickRandomStarterSkin();

struct SkinEntry {
    std::string  name;         // pak stem = package name = wire name
    std::wstring previewPath;  // sibling <name>.png/.bmp; empty = no preview tile
};

// [A-Za-z0-9_-], 1..48 chars. Boundary rule for ini reads, wire receives and
// pak-dir scans alike.
bool IsValidSkinName(const std::string& name);

// v94 builtin skins: skin name -> full game object path of a body mesh on the
// player-compatible kerfurOmegaV1_Skeleton rig (the rig our converter template
// kerfurOmega_KelSkin binds -- proven player-compatible in-game). nullptr when
// `name` is not a builtin. Pure table lookup; any thread.
const wchar_t* BuiltinSkinPath(const std::string& name);

// The catalog: entry 0 = dr_kel, then the builtin kerfur skins, then one entry
// per *.pak in the LogicMods votv-coop folder (invalid stems skipped + logged;
// a pak shadowing a builtin name is skipped -- builtins win). First call scans;
// rescan=true re-scans (the browser's Refresh / tab open). RENDER-THREAD ONLY
// (the F1 browser); other threads use names, not the catalog.
const std::vector<SkinEntry>& Entries(bool rescan = false);

// <game>/VotV/Content/Paks/LogicMods/votv-coop (derived from the DLL's own
// module dir: Binaries/Win64 -> VotV). Empty on resolve failure.
std::wstring PakDir();

}  // namespace coop::skins
