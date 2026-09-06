// coop/player/skin_registry.h -- the installed body-skin catalog behind the F1 browser.
//
// Three sources, one namespace: "dr_kel" (entry 0) is the native stock body and loads
// no asset; the builtin skins are the game's own kerfurOmegaV1_Skeleton bodies, addressed
// by game asset path, so no pak is needed; converter paks under LogicMods carry the rest.
// A single-skin pak's stem is both the skin name and the package name the runtime loads.
// A BUNDLE pak carries several skins, and since assets are addressed by their internal
// paths its filename says nothing: kSkinBundles maps a bundle stem to the members it
// provides. A preview tile is a sibling <name>.png or .bmp named after the MEMBER.
//
// The catalog half is filesystem only and Entries() has one caller, the UI on the render
// thread, which is what makes the absence of a lock correct. The usability half loads
// assets, so it is game-thread-only with storage of its own. A skin name is both a
// LoadObject path component and a reliable-payload field: one rule validates it at the
// ini read, the wire receive and the pak scan alike.

#pragma once

#include <string>
#include <vector>

namespace coop::skins {

// The native (pak-less) skin: Dr. Kel, the stock player body.
inline constexpr const char* kNativeSkinName = "dr_kel";


// Pick a random skin for a NEW player identity from the curated starter list, filtered to
// what the installed paks actually PROVIDE. "Provides" is the load-bearing word: it asks
// whether some pak under LogicMods carries the skin -- its own <name>.pak, or a bundle that
// lists it -- never whether <name>.pak exists on disk. Asking the filesystem fails silently
// against a shared pak: no candidate file exists, the list comes back empty, and every new
// identity falls back to the stock body while looking like a content decision. Returns
// kNativeSkinName when none of the list is available, never a pak name: on an install
// carrying no starter pak, a pak named "default" is one more name that cannot load. Boot
// thread (the config read); touches the filesystem once.
std::string PickRandomStarterSkin();

struct SkinEntry {
    std::string  name;         // the SKIN name = package name = wire name (a single-skin
                               // pak's stem, or a bundle member -- never a bundle's stem)
    std::wstring previewPath;  // sibling <name>.png/.bmp; empty = no preview tile
};

// Does a skin's mesh actually EXIST on this machine? A pak is not a skin: the scan offers
// the stem of every .pak under LogicMods, so ANOTHER MOD's pak -- DebugMod, FusionPatch_P --
// is listed and resolves to no skin anywhere. Picking one used to persist and announce it,
// which diverged every peer to the native body while the local one silently kept the skin it
// already wore.
//
// THE VERDICT IS NOT A FIELD ON `SkinEntry`, and that is the whole design. Only the game
// thread may ask the engine, and `Entries()` is render-thread-only by the rule above, so a
// verdict living in the catalog would mean the game thread reading `e.name` and writing
// `e.usable` in a vector the panel can `clear()` and reallocate under it -- a heap
// use-after-free one Refresh click wide. So nothing is shared: NAMES travel render -> game
// BY VALUE, once per scan, and VERDICTS travel game -> render through a small store of their
// own, leaving Entries()'s single-caller discipline untouched.
enum class Usable : uint8_t { Unknown, Yes, No };

// RENDER THREAD (the panel), after a scan: hand the game thread the names to judge. Copies.
void RequestUsabilityScan(std::vector<std::string> names);

// GAME THREAD. Answer at most `budget` outstanding names. Cheap and self-limiting: each name
// is asked once per scan and the engine caches the load.
void ResolvePending(int budget);

// Either thread. `Unknown` means NOT YET ASKED -- never "bad"; a caller that hides Unknown
// would blank the picker for the first frames after a scan.
Usable UsabilityOf(const std::string& name);

// [A-Za-z0-9_-], 1..48 chars. Boundary rule for ini reads, wire receives and
// pak-dir scans alike.
bool IsValidSkinName(const std::string& name);

// Builtin skins: skin name -> the full game object path of a body mesh on the
// player-compatible kerfurOmegaV1_Skeleton rig -- the rig our converter template
// kerfurOmega_KelSkin binds. nullptr when `name` is not a builtin. Pure table lookup;
// any thread.
const wchar_t* BuiltinSkinPath(const std::string& name);

// The catalog: entry 0 = dr_kel, then the builtin kerfur skins, then the skins the
// installed paks provide -- one entry per single-skin pak's stem, or one entry per
// MEMBER for a bundle pak (invalid stems skipped + logged; a pak shadowing a builtin
// name is skipped -- builtins win). Scans every LogicMods subfolder. First call scans;
// rescan=true re-scans (the browser's Refresh / tab open). RENDER-THREAD ONLY
// (the F1 browser); other threads use names, not the catalog.
const std::vector<SkinEntry>& Entries(bool rescan = false);

}  // namespace coop::skins
