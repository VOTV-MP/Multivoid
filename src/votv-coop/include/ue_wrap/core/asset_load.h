// ue_wrap/asset_load.h -- load a cooked asset from a mounted .pak by /Game path.
//
// VOTV ships URyRuntimeObjectHelpers (a UBlueprintFunctionLibrary) whose
// LoadObject(FString) synchronously loads + returns a UObject by its /Game/...
// object path. UE4 auto-mounts every .pak under Content/Paks/ at startup, so a
// pak we drop under Content/Paks/LogicMods/ is resident by the time the world is
// up (research/findings/architecture-audits/votv-mp-pak-mount-feasibility-2026-05-25.md). This wraps
// that reflected call -- pure engine access, no coop/gameplay logic (principle
// 7). RULE 3: no UE4SS, no native pak API; the game's own plugin does the mount.
//
// Game thread only (goes through ProcessEvent).

#pragma once

namespace ue_wrap::asset_load {

// Load + return the UObject at `fullObjectPath` (e.g.
// L"/Game/Mods/VOTVCoop/hl_einstein_v1sc.kerfurOmega_KelSkin"), or nullptr if the
// RyRuntime plugin is absent, the pak isn't mounted, or the path doesn't
// resolve. Synchronous (forces the package load). The caller caches the
// result; this performs no caching of its own.
void* LoadObjectByPath(const wchar_t* fullObjectPath);

}  // namespace ue_wrap::asset_load
