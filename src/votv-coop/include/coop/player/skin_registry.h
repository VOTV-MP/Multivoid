// coop/player/skin_registry.h -- the installed body-skin catalog (F1 browser source).
//
// Three skin sources, one namespace:
//   - "dr_kel" (entry 0): the native stock body, no asset load (pristine-mesh revert).
//   - BUILTIN skins (v94, user 2026-07-02 "добавь скин керфура робота"): the game's
//     own kerfurOmegaV1_Skeleton bodies (the anthro robot kerfur + its skin variants),
//     loaded by their game asset path -- no pak needed, materials come with the mesh.
//   - Converter paks under <game>/VotV/Content/Paks/LogicMods/<any subfolder>/:
//     for a SINGLE-skin pak the filename stem IS the skin name AND the package
//     name the runtime loads (/Game/Mods/VOTVCoop/<name>.kerfurOmega_KelSkin --
//     every pak from tools/client_model/ splices into that template object).
//   - BUNDLE paks (2026-08-29, user: "нужен общий пак scientists.pak"): one pak
//     carrying SEVERAL skins. The loader never keys on the pak filename (assets
//     are addressed by their internal /Game/Mods/VOTVCoop/<name> paths, all of
//     which UE mounts at startup), so presence is the only thing a bundle
//     changes: kSkinBundles maps the pak stem to the member skin names it
//     provides. Member previews stay sibling files by MEMBER name
//     (walter_v1sc.png next to scientists.pak).
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
// skins (PickRandomStarterSkin) is available from the installed paks. Written to
// multivoid.ini player_skin= on first launch. (Since 2026-08-29 the identity itself
// is no longer minted beside it -- see coop/net/peer_identity.h; and since the same
// day the four starter scientists arrive in ONE bundle, so "is this skin installed"
// is asked of the registry, never of `<name>.pak` on disk.)
inline constexpr const char* kDefaultSkinName = "hl_einstein_v1sc";

// v95 random starter (user 2026-07-02: "для НОВЫХ пиров случайный скин из списка"):
// pick a random skin for a NEW player identity from the curated starter list, filtered
// to what the INSTALLED PAKS ACTUALLY PROVIDE (a pick that cannot load would pin the
// new player to the native fallback body). "Provides" is the load-bearing word: it asks
// whether some pak under LogicMods carries the skin -- its own `<name>.pak`, or a bundle
// that lists it -- never whether `<name>.pak` exists on disk. Asking the filesystem is
// how the 2026-08-23 lesson's silent failure happens: with a shared pak no candidate
// file exists, the list comes back empty, and every new identity falls back to
// kDefaultSkinName while looking like a content decision.
// Returns kDefaultSkinName when none of the list is available. Boot thread (config
// read path) -- touches the filesystem once.
std::string PickRandomStarterSkin();

struct SkinEntry {
    std::string  name;         // the SKIN name = package name = wire name (a single-skin
                               // pak's stem, or a bundle member -- never a bundle's stem)
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

// The catalog: entry 0 = dr_kel, then the builtin kerfur skins, then the skins the
// installed paks provide -- one entry per single-skin pak's stem, or one entry per
// MEMBER for a bundle pak (invalid stems skipped + logged; a pak shadowing a builtin
// name is skipped -- builtins win). Scans every LogicMods subfolder. First call scans;
// rescan=true re-scans (the browser's Refresh / tab open). RENDER-THREAD ONLY
// (the F1 browser); other threads use names, not the catalog.
const std::vector<SkinEntry>& Entries(bool rescan = false);

}  // namespace coop::skins
