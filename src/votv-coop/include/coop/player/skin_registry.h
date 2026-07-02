// coop/player/skin_registry.h -- the installed body-skin catalog (F1 browser source).
//
// A "skin" is one converter pak in <game>/VotV/Content/Paks/LogicMods/votv-coop/:
// the pak filename stem IS the skin name AND the package name the runtime loads
// (/Game/Mods/VOTVCoop/<name>.kerfurOmega_KelSkin -- every pak from
// tools/client_model/ splices into that template object). A sibling <name>.png
// or <name>.bmp is the browser preview tile (user convention 2026-07-02:
// previews live NEXT to the pak). Entry 0 is always "dr_kel" -- the native
// stock body, no pak.
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

// The factory default for a NEW player identity (user 2026-07-02: a new player
// starts as the CURRENT scientist). Written to votv-coop.ini player_skin= on
// first launch, right where player_guid= is generated.
inline constexpr const char* kDefaultSkinName = "hl_einstein_v1sc";

struct SkinEntry {
    std::string  name;         // pak stem = package name = wire name
    std::wstring previewPath;  // sibling <name>.png/.bmp; empty = no preview tile
};

// [A-Za-z0-9_-], 1..48 chars. Boundary rule for ini reads, wire receives and
// pak-dir scans alike.
bool IsValidSkinName(const std::string& name);

// The catalog: entry 0 = dr_kel, then one entry per *.pak in the LogicMods
// votv-coop folder (invalid stems skipped + logged). First call scans;
// rescan=true re-scans (the browser's Refresh / tab open). RENDER-THREAD ONLY
// (the F1 browser); other threads use names, not the catalog.
const std::vector<SkinEntry>& Entries(bool rescan = false);

// <game>/VotV/Content/Paks/LogicMods/votv-coop (derived from the DLL's own
// module dir: Binaries/Win64 -> VotV). Empty on resolve failure.
std::wstring PakDir();

}  // namespace coop::skins
