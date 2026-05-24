# VOTV_MP pak-mount feasibility (standalone proxy DLL)

**Date:** 2026-05-25
**Status:** RE + assessment only -- no code shipped.
**Question:** can our standalone proxy DLL (`votv-coop.dll`, mounted via the
`xinput1_3.dll` hijack) load UMG widget assets from a sibling
`votv-coop.pak` at runtime, without UE4SS / VoidMod / any other mod
framework?

**Verdict (one-line):** **FEASIBLE without VoidMod** -- VOTV ships two
reflection-callable `MountPakFile` Blueprint libraries AND VOTV's pak
discovery auto-mounts every `.pak` it finds under `Content/Paks/` at
startup; loading widget classes from our pak is a `URyRuntimeObjectHelpers::LoadObject`
or `UPakLoaderLibrary::GetPakFileClass` reflection call away. Confirmed by
VoidTogether's pak (drops cleanly in the same install) being unsigned,
unencrypted, UE4.27 pak format v11, with assets visible in the index.

---

## 1. UE4.27 pak-mount API surface

### 1.1 What we have via reflection (Blueprint-callable, zero AOB scanning)

VOTV ships TWO independent plugins that expose pak-mount + asset-load
helpers as `UBlueprintFunctionLibrary` UFunctions. Both surface in our CXX
header dump and are therefore callable via the existing `R::FindClass` +
`R::FindFunction` + `ParamFrame` plumbing (see
`src/votv-coop/src/ue_wrap/call.cpp`, `engine.cpp::SpawnObject`).

**Plugin A: `PakLoader` (`Game_0.9.0n/.../CXXHeaderDump/PakLoader.hpp`).**
The Rama PakLoader plugin. `class UPakLoaderLibrary : public UBlueprintFunctionLibrary`
with these reflection-callable static UFunctions:

```cpp
bool MountPakFileEasy(FString PakFilename);                 // single-arg mount
bool MountPakFile(FString PakFilename, FString MountPath);  // explicit mount point
bool UnmountPakFile(FString PakFilename);
bool RegisterEncryptionKey(FString Guid, FString AesKey);
bool IsValidPakFile(FString PakFilename, int64& PakSize);
TArray<FString> GetMountedPakFilenames();
TArray<FString> GetFilesInPak(FString PakFilename, bool bUAssetOnly);
UClass* GetPakFileClass(FString Filename);                  // !!! load class direct
class UObject* GetPakFileObject(FString Filename);          // load object direct
void LoadPakAssetRegistryFile(FString AssetRegistryFile);
void RegisterMountPoint(FString RootPath, FString ContentPath);
```

`GetPakFileClass(FString)` is what we want for UMG widgets -- it returns a
`UClass*` for a cooked asset path. Combined with our existing
`SpawnObject(class, outer)` (= `UGameplayStatics::SpawnObject`, the
reflected `NewObject` equivalent we already use for the runtime nameplate),
this is the whole pipeline.

**Plugin B: `RyRuntime`
(`Game_0.9.0n/.../CXXHeaderDump/RyRuntime.hpp`).** The RyanPakHelpers /
RyRuntime plugin. Three relevant Blueprint libraries:

```cpp
class URyRuntimePakHelpers : public UBlueprintFunctionLibrary {
    bool MountPakFile(FString pakFilePath);
    bool UnmountPakFile(FString pakFilePath);
    void GetMountedPakFilenames(TArray<FString>& mountedPakFilenames);
};

class URyRuntimeObjectHelpers : public UBlueprintFunctionLibrary {
    class UObject* LoadObject(FString fullObjectPath);
    class UObject* LoadObjectFromPackage(class UPackage* Package, FString objectName);
    class UPackage* FindOrLoadPackage(FString PackageName);
    void RegisterMountPoint(FString RootPath, FString ContentPath);
    bool MountPointExists(FString RootPath);
    UClass* GetParentClass(UClass* Class);
    void CreateObject(UClass* objectClass, class UObject* Outer, class UObject*& objectOut);
};

class URyRuntimePlatformHelpers : public UBlueprintFunctionLibrary {
    bool fileExists(FString FilePath);                      // graceful-degradation gate
    void PathInfo(FString fileSystemPath, bool& Exists, bool& isFile, ..., int64& fileSize);
};
```

`URyRuntimeObjectHelpers::LoadObject(FString fullObjectPath)` is the
classic synchronous "load by `/Game/...` path" call. This is the cleanest
fit for our use: pass `"/Game/Mods/VOTVCoop/WBP_VOTVCOOP_Nameplate.WBP_VOTVCOOP_Nameplate_C"`
and get a `UClass*` back.

### 1.2 The CXX-header-dump caveat (confirmed not a blocker)

The dump only shows reflection-exposed UFunctions, not raw C++ APIs. The
canonical native `FPakPlatformFile::MountPakFile` /
`FCoreDelegates::OnPakFileMounted` / `FPakFile::Initialize` symbols are
**not** in the dump -- they're native-only. To call them we'd need AOB
signatures + a typed function pointer (the path our `reflection::Resolve`
takes for `GUObjectArray` / `FName::ToString` / `ProcessEvent`).

**We don't need to.** VOTV's two BP libraries above wrap exactly that
native API. Calling `URyRuntimePakHelpers::MountPakFile` via reflection
ends up at the same `FPakPlatformFile::MountPakFile` we'd otherwise AOB-
scan, with zero version-drift risk on our side (the plugin owns the
version-specific glue).

### 1.3 Both mount routes are independently sufficient

We have TWO independently maintained, independently shipped plugins
exposing the same primitive. If one drifts on a future game update we still
have the other; if both drift we fall back to AOB-scanning
`FPakPlatformFile::MountPakFile` directly. The blast radius is small.

---

## 2. Auto-mount: do we even need to call `MountPakFile`?

**Short answer:** no -- UE4's startup pak scanner picks up our pak
automatically if we drop it in the right directory. This is the simplest
deployment path and the one we should ship.

### 2.1 Evidence from VoidTogether and UE4SS-BPModLoader

`reference/RE-UE4SS/assets/Mods/BPModLoaderMod/Scripts/main.lua` walks
`Content/Paks/LogicMods/<modname>/<modname>.pak` and **never calls
`MountPakFile`** -- the script only enumerates already-mounted assets:

```lua
local LogicModsDir = Dirs.Game.Content.Paks.LogicMods
-- ... iterate .pak files in subdirs ...
-- Then directly:
local ModClass = AssetRegistryHelpers:GetAsset({PackageName = ..., AssetName = ...})
local Actor = World:SpawnActor(ModClass, {}, {})
```

It assumes the pak is already mounted -- because UE4's startup pak scan
walks `Content/Paks/` recursively and mounts every `.pak` found there. BP
mods on Thunderstore (including the entire `LogicMods/` ecosystem) ride
this path; thousands of UE4.27 games confirm it works.

VoidTogether's own pak is at
`reference/voidtogether-client-extracted/pak/VoidTogether.pak` -- it's
shipped to be dropped under `Content/Paks/LogicMods/VoidTogether/` (the
`manifest.json` lists `Gatohost-VoidMod-2.1.0` as a dependency for the
loader, but the **pak loading** is UE4's job, not VoidMod's).

### 2.2 UE4.27 pak-scan path

UE4.27's `FPakPlatformFile::Initialize` runs early in
`FEngineLoop::PreInit` -- before our proxy DLL would have any business
touching reflection. It calls `FindAllPakFiles` which walks the platform
paks directories (`<Project>/Content/Paks/*`, plus `~mods/` chunk dirs)
recursively and queues every `.pak` for mount, then mounts them. So **by
the time our boot thread runs `ue_wrap::reflection::RunHealthCheck()`, any
pak we dropped under `Content/Paks/` is already mounted and visible to
`GUObjectArray`**.

### 2.3 Recommended pak placement

`Game_0.9.0n/WindowsNoEditor/VotV/Content/Paks/LogicMods/votv-coop/votv-coop.pak`.

- Under `LogicMods/` -- the Thunderstore-standard subdir for third-party
  paks; convention more than necessity (UE4 mounts any subdir).
- Subfolder = pak name; matches the `BPModLoaderMod` layout.
- Asset mount root = `/Game/Mods/VOTVCoop/...` (matches VoidTogether's
  `/Game/Mods/VoidTogether/...`).

`Content/Paks/LogicMods/` already exists in the install (empty), so the
directory contract is established by the game itself.

### 2.4 Manual mount as a fallback only

If we ever ship a pak from a *non-standard* location (e.g. dropped next to
`votv-coop.dll` in `Binaries/Win64/`), we'd call
`URyRuntimePakHelpers::MountPakFile(FString)` from our boot path. But this
adds a moving part we don't need. **Recommend: auto-mount via standard
location; reserve manual mount for future debug overrides only.**

---

## 3. Load-time sequencing

### 3.1 Current proxy boot order

`src/votv-coop/src/bootstrap/dllmain.cpp::BootThread`:

1. `DllMain` (DLL_PROCESS_ATTACH) -> spawn `BootThread` off the loader lock.
2. `BootThread`: `WriteMarker` -> `log::Init` ->
   `reflection::RunHealthCheck` (resolves `GUObjectArray` / `FName::ToString` /
   `ProcessEvent` via AOB).
3. `game_thread::Install` -> install ProcessEvent PRE-hook to obtain a
   game-thread callback context.
4. `harness::Start` -> autonomous skip-menu test, OR (in the shipping flow)
   the coop session boots and waits for the world.

`xinput_proxy.cpp::LoadPayload` only runs `LoadLibraryW(votv-coop.dll)`;
it lives in the **very-early DLL load** window (xinput1_3.dll is one of
the first DLLs Windows loads for the shipping exe).

### 3.2 Where the pak is already there

UE4's pak mount happens inside `FEngineLoop::PreInit`. Our proxy is
mapped before `WinMain` even starts. By the time the engine reaches the
"game thread is processing events" state -- which is what we wait for in
the harness's retry loop -- every pak under `Content/Paks/` is mounted and
all its asset packages are discoverable.

The retry pattern we already have (`NetPumpTick` walks GUObjectArray, retries
"find class X" if not loaded yet -- see `harness.cpp::NetPumpTick`) covers
the pak-content case identically: our `WBP_VOTVCOOP_Nameplate_C` is just
another `UClass` in `GUObjectArray` once the pak's package is loaded. So:

- Eager: `URyRuntimeObjectHelpers::LoadObject` on a known path forces the
  package to load synchronously when called.
- Lazy: `R::FindClass(L"WBP_VOTVCOOP_Nameplate_C")` only matches once the
  package has been touched. To trigger it, call `LoadObject` once at
  game-ready -- the package then stays resident.

### 3.3 The "is the pak there?" gate

We must NOT call any reflection-based pak helper from `BootThread`
directly: those calls go through `ProcessEvent`, which is game-thread-only.
The existing `game_thread::Post([...])` queue handles that. So the load
sequence becomes:

```
BootThread:
    reflection::Resolve();
    game_thread::Install();
    game_thread::Post([] {
        if (!engine::EnsureCoopPakLoaded()) {
            // Pak missing or asset load failed -- fall back to programmatic widget.
            engine::SetUseProgrammaticWidgets(true);
        }
    });
    harness::Start();
```

`EnsureCoopPakLoaded` runs on the game thread the first time it's called
post-PreInit (so after auto-mount); it does the `LoadObject` call and
caches the resulting `UClass*` pointers for our widgets.

---

## 4. Pak signing / encryption -- not a blocker

### 4.1 VOTV's main pak header analysis

Read at offset (file_size - 221), `VotV-WindowsNoEditor.pak`:

```
0000: 00 e1 12 6f 5a    bEncryptedIndex=0, Magic 0x5A6F12E1     [UE4 pak]
0005: 0b 00 00 00       Version = 11                            [UE 4.27]
0009: 00 96 45 de e6 01 00 00 00   Index offset
0012: 00 c6 ca 09 00 00 00 00 00   Index size (~640 KB)
...: SHA1, then padded zeros for compression methods (NONE used)
```

**`bEncryptedIndex` byte = 0 -- index is plaintext.** No
`EncryptionKeyGuid` is set. VOTV does NOT enforce signing or encryption on
paks. The platform pak file (`FPakPlatformFile`) will mount any unsigned
pak whose footer parses cleanly.

### 4.2 VoidTogether's pak header analysis

Read at offset (17712757 - 221) in `VoidTogether.pak`:

```
0000: 00 e1 12 6f 5a    bEncryptedIndex=0, Magic 0x5A6F12E1     [UE4 pak]
0005: 0b 00 00 00       Version = 11                            [UE 4.27]
0009: 00 6f bc 0d 01 00 00 00   Index offset (~282 MB)
0012: 00 bd 27 00 00 00 00 00   Index size (~2.5 MB)
...: SHA1, then "Oodle" compression method
```

Same format, same version, also unencrypted index. Compressed with Oodle
(VOTV ships Oodle as part of the engine plugin DLLs in
`Engine/Binaries/ThirdParty/Oodle/`). The asset paths
(`/Game/Mods/VoidTogether/...`) are readable in plain bytes inside the
index: `Interfaces/VT_Util`, `Structs/PacketAuth`, etc.

If VT's unsigned pak mounts under VoidMod, it mounts under our setup too
-- VoidMod doesn't add cryptographic privilege; it's just a Lua-script
runner. The pak mount happens at the engine level.

### 4.3 If VOTV ever turns on signing

Unlikely (would break every existing mod overnight, and the VOTV dev is
mod-friendly per VoidMod's existence), but if it ever happens:
`UPakLoaderLibrary::RegisterEncryptionKey(FString Guid, FString AesKey)` is
reflection-callable, so we'd just register the public key from our DLL
before the engine attempts to mount. That's an additional ~10 LOC, not an
architectural change.

---

## 5. Cooking our own `.pak` -- toolchain reality

### 5.1 The realistic path

**UE 4.27.2 editor install via Epic Games Launcher** (Add Versions -> 4.27).
Roughly **35-50 GB** for engine binaries + Win64 cooked content target;
add ~20 GB for the C++ source if we want crash-debugging symbols. So
~80 GB on disk realistically.

This is a one-time install for the maintainer cooking the pak; users
download the pre-cooked pak alongside the DLL.

### 5.2 Alternatives evaluated

- **`UE4Editor-Cmd.exe` CookCommandlet alone:** ships in the same install
  -- you can't get the Cmd without the editor. Saves no space.
- **UAssetGUI / UAssetAPI:** edits existing .uasset files; CAN create
  simple new ones (text/image references), CANNOT create new
  `WidgetBlueprintGeneratedClass` containing real widget tree + bindings
  without a full editor pipeline. Brittle on engine bumps. **Not viable**
  for a `WBP_*` with bound Slate composition.
- **Hand-crafted .uasset:** theoretically possible (the format is
  documented), in practice we'd hand-author each FProperty serialization
  table -- one engine point release breaks it. **Not viable.**
- **Use VOTV's editor + dev cook:** the dev (Eeris) cooks the game in
  4.27. No public engine source. Not an option for outsiders.

**Conclusion: full editor install is the only realistic cook path.**

### 5.3 Build pipeline shape (hybrid DLL + pak)

```
VOTV_MP/
  src/votv-coop/                    -- existing C++ source (CMake)
  tools/votv-coop-content/          -- NEW: UE 4.27 content project
    VotvCoopContent.uproject
    Content/Mods/VOTVCoop/
      WBP_Nameplate.uasset
      WBP_ChatBubble.uasset
      WBP_ServerBrowser.uasset
      M_NameplateTranslucent.uasset
  build/votv-coop/                  -- DLL build output (existing)
  build/votv-coop-content/          -- NEW: cooked pak output
    WindowsNoEditor/.../votv-coop.pak
```

Build step:
1. `cmake --build build/votv-coop --config Release` -- DLL (~5 sec).
2. (When content changed) `UE4Editor-Cmd.exe VotvCoopContent.uproject
   -run=Cook -targetplatform=WindowsNoEditor -unattended` -- pak
   (~30-60 sec for a small mod, ~3-5 min for cold cache).
3. `tools/build-pak.ps1` packages the cooked content into a single
   `votv-coop.pak` via `UnrealPak.exe -Create=...`.
4. `tools/deploy-loader.ps1` extended with `-Pak` switch: copies
   `xinput1_3.dll` + `votv-coop.dll` to `Binaries/Win64/` AND
   `votv-coop.pak` to `Content/Paks/LogicMods/votv-coop/`.

Content-iteration loop is 30-60 s; DLL-iteration loop stays ~5 s. Good
asymmetry -- most of our work is C++ + reflection, not asset authoring.

### 5.4 Cost-benefit by widget

| Widget                  | Runtime build cost (status quo)                              | Pak cost saved                                | Verdict             |
|-------------------------|--------------------------------------------------------------|-----------------------------------------------|---------------------|
| Nameplate "Player 2"    | DONE in `coop/nameplate.cpp` via `BuildTextWidget`           | Tiny -- it's one TextBlock                    | KEEP programmatic   |
| HUD feed (top-right)    | DONE in `coop/event_feed.cpp` via shared `BuildTextWidget`   | Tiny                                          | KEEP programmatic   |
| Chat bubble (above head, with avatar, fade) | not built; ~150 LOC of UMG composition + tween logic | Designer-friendly; native-styled         | **PAK candidate**   |
| Multiplayer menu (Host / Connect / Browser) | not built; ~500-800 LOC of UMG composition           | Native look, native styling, layout in editor | **PAK candidate**   |
| Server browser list     | not built; complex ListView with per-row bindings           | UMG ListView + entry widget                    | **PAK candidate**   |

The pak pays off where the widget is complex enough that hand-authoring in
C++ becomes painful and where the user-visible result is part of the
mod's polish (server browser, menu). The two existing programmatic widgets
should stay programmatic (RULE 2: no parallel paths for one concept --
either pak everything or pak only what's not yet built).

---

## 6. Asset references from the C++ DLL

### 6.1 Loading the widget class

Once the pak is auto-mounted at `/Game/Mods/VOTVCoop/`, our code needs a
`UClass*` for `WBP_VOTVCOOP_Nameplate_C`. Two paths, both reflection-only:

**Path A (synchronous, blocking, simplest):** `URyRuntimeObjectHelpers::LoadObject`.

```cpp
// Inside engine.cpp, called once on game-thread after game ready:
void* g_coopNameplateClass = nullptr;

bool EnsureCoopNameplateClass() {
    if (g_coopNameplateClass) return true;
    void* cdo = R::FindClassDefaultObject(L"RyRuntimeObjectHelpers");
    if (!cdo) return false;
    void* fn = R::FindFunction(R::ClassOf(cdo), L"LoadObject");
    if (!fn) return false;
    std::wstring path = L"/Game/Mods/VOTVCoop/WBP_VOTVCOOP_Nameplate.WBP_VOTVCOOP_Nameplate_C";
    R::FString fs{ path.data(), int32_t(path.size()) + 1, int32_t(path.size()) + 1 };
    ParamFrame f(fn);
    f.SetRaw(L"fullObjectPath", &fs, sizeof(fs));
    if (!Call(cdo, f)) return false;
    g_coopNameplateClass = f.Get<void*>(L"ReturnValue");
    return g_coopNameplateClass != nullptr;
}
```

The path syntax is the standard `/Mount/Subpath/Package.AssetName_C` form;
the trailing `_C` is what makes it the BP-generated class (vs the BP asset
itself).

**Path B (just-look-up-after-mount):** once the pak is mounted, the
package is discoverable but not necessarily resident. `R::FindClass(L"WBP_VOTVCOOP_Nameplate_C")`
matches only after the package has been touched. So Path A is still
necessary to FORCE the load; we then cache the pointer.

### 6.2 Instancing the widget

We already do this for our programmatic widget (engine.cpp:463 `SpawnObject`):

```cpp
void* widget = SpawnObject(g_coopNameplateClass, /*outer=*/ actor);
// then SetWidget on the WidgetComponent (engine.cpp:591), etc.
```

`UGameplayStatics::SpawnObject(objectClass, Outer)` = the reflected
`NewObject<T>(Outer, Class)`. The cooked widget's `Initialize` /
`Construct` is dispatched automatically by UMG when it's attached to a
WidgetComponent or added to viewport (`AddToViewport` is already wired in
`engine.cpp::ResolveScreenWidgetFns`).

### 6.3 Reading / writing widget properties from C++

If the cooked widget has a `NameText` `UTextBlock` child, we reach it
either by:

- Iterating `WidgetTree.AllWidgets[]` (an FProperty on UWidgetTree) and
  matching by `NameProperty == "NameText"`.
- `FindObject(widget, L"NameText")` -- once the widget instance is
  constructed, all named children are subobjects findable by name.

Then `SetTextOnBlock(child, L"Player 2")` (engine.cpp:474) is the same
`Conv_StringToText` + `UTextBlock::SetText` path we already use. **Zero
new ProcessEvent plumbing** for the leaf-text update; only the widget
composition is paked.

---

## 7. Graceful degradation

The pak is OPTIONAL by design. The mod must boot cleanly when the user
installs only the DLL (e.g. drops `xinput1_3.dll` + `votv-coop.dll` into
the install but forgets `votv-coop.pak`).

```cpp
// engine.cpp pseudo:
namespace coop_pak {
    bool g_loaded = false;

    void* g_nameplateClass    = nullptr;  // /Game/Mods/VOTVCoop/WBP_VOTVCOOP_Nameplate_C
    void* g_chatBubbleClass   = nullptr;
    void* g_serverBrowserCls  = nullptr;

    bool TryLoad() {
        // Heuristic gate: try LoadObject; if any one fails, treat the pak as absent.
        g_nameplateClass = LoadObjectByPath(
            L"/Game/Mods/VOTVCoop/WBP_VOTVCOOP_Nameplate.WBP_VOTVCOOP_Nameplate_C");
        if (!g_nameplateClass) return false;
        // ...load the rest...
        g_loaded = true;
        return true;
    }
} // namespace coop_pak

// Call site:
void* MakeNameplate(void* outer, const wchar_t* text) {
    if (coop_pak::g_loaded && coop_pak::g_nameplateClass) {
        void* w = SpawnObject(coop_pak::g_nameplateClass, outer);
        // populate fields by name on the cooked widget...
        return w;
    }
    // Fallback to existing programmatic build (the path we use today):
    return BuildTextWidget(outer, text, FLinearColor{1, 1, 1, 1}, 14, /*Center*/ 1).root;
}
```

A `URyRuntimePlatformHelpers::fileExists(FString)` precheck on
`<install>/VotV/Content/Paks/LogicMods/votv-coop/votv-coop.pak` could
short-circuit `TryLoad` for cleaner logging, but the LoadObject-returns-
null path is already robust.

**Note on RULE 2 (no migration baggage):** programmatic+pak parallel
paths for the SAME widget violates RULE 2. The plan above uses
programmatic for widgets we already built; pak for widgets not yet built.
There's no widget with both implementations alive at the same time.

---

## 8. Minimal proof-of-concept sketch (~40 LOC)

```cpp
// Plumbing additions to engine.cpp; SDK profile gets the new class names:

// sdk_profile.h additions (P::name::):
inline constexpr const wchar_t* RyObjectHelpersClass = L"RyRuntimeObjectHelpers";
inline constexpr const wchar_t* LoadObjectFn         = L"LoadObject";
inline constexpr const wchar_t* CoopNameplatePath    =
    L"/Game/Mods/VOTVCoop/WBP_VOTVCOOP_Nameplate.WBP_VOTVCOOP_Nameplate_C";

// engine.cpp:
void* g_ryObjCdo = nullptr, *g_ryLoadObjectFn = nullptr;
void* g_coopNameplateClass = nullptr;

bool ResolveCoopAssetLoaderFns() {
    if (!g_ryObjCdo) g_ryObjCdo = R::FindClassDefaultObject(P::name::RyObjectHelpersClass);
    if (g_ryObjCdo && !g_ryLoadObjectFn) {
        if (void* c = R::ClassOf(g_ryObjCdo))
            g_ryLoadObjectFn = R::FindFunction(c, P::name::LoadObjectFn);
    }
    return g_ryObjCdo && g_ryLoadObjectFn;
}

void* LoadCoopAssetClass(const wchar_t* fullObjectPath) {
    if (!ResolveCoopAssetLoaderFns()) return nullptr;
    std::wstring p(fullObjectPath);
    R::FString fs{ p.data(), int32_t(p.size()) + 1, int32_t(p.size()) + 1 };
    ParamFrame f(g_ryLoadObjectFn);
    f.SetRaw(L"fullObjectPath", &fs, sizeof(fs));
    if (!Call(g_ryObjCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

// Spawn a cooked nameplate widget; falls back to programmatic.
void* SpawnCoopNameplate(void* outer, const wchar_t* text) {
    if (!g_coopNameplateClass) g_coopNameplateClass = LoadCoopAssetClass(P::name::CoopNameplatePath);
    if (g_coopNameplateClass) {
        void* w = SpawnObject(g_coopNameplateClass, outer);
        if (w) {
            // The cooked widget has a UTextBlock named "NameText" inside its WidgetTree.
            // Walk the tree once after construction, cache the pointer per-instance:
            void* tb = R::FindObject(L"NameText", P::name::TextBlockClass);
            if (tb) SetTextOnBlock(tb, text);
            return w;
        }
    }
    // Fallback to the existing programmatic builder.
    return BuildTextWidget(outer, text, FLinearColor{1.f, 1.f, 1.f, 1.f}, 14, 1).root;
}
```

Net change: ~40 LOC in `engine.cpp`, ~3 lines in `sdk_profile.h`. **No
loader changes, no AOB scanning, no native function pointers.**

---

## 9. Risk list

### High

- **Asset format drift on VOTV updates.** If VOTV moves to UE 4.28 or a
  major engine bump, our cooked `.pak` (cooked for 4.27) may be unreadable
  -- the engine version is baked into the pak header. Mitigation: our
  version-tagging rule already pins the mod to a target game version;
  recook on game bumps. Same exposure VoidTogether / every BP mod has.

### Medium

- **Editor install footprint (~80 GB).** Real cost for the maintainer
  cooking the pak. Acceptable -- one-time, on one machine. Users download
  the pre-cooked pak (likely 2-5 MB for a few widgets).
- **Cook-time UE asset reference instability.** If a cooked widget hard-
  refs a VOTV asset (`/Game/Materials/M_Whatever`), the asset path must
  exist in VOTV's main pak; deletion in a future game patch breaks our
  widget. Mitigation: cook our pak with our OWN materials only (or only
  ref engine-built-in assets like `Engine/Content/EngineMaterials/*`).
- **Build-pipeline complexity.** Two artefacts to ship (DLL + pak)
  instead of one. The CMake side stays small; the UE editor side is a
  separate project. Mitigation: keep the content project tiny and only
  rebuild the pak when content changes (track via Git modification time).

### Low

- **Pak load order.** UE4 mounts paks in the order it discovers them.
  Our pak should NOT override any VOTV asset path; we mount under
  `/Game/Mods/VOTVCoop/` so collisions are structurally impossible. (If
  we ever wanted to OVERRIDE a VOTV asset, that's anti-pattern A6 anyway
  -- not what we're doing.)
- **Encryption / signing change.** If VOTV ever enables pak signing,
  `RegisterEncryptionKey` is reflection-callable and we adapt. Unlikely.
- **Subobject lookup by name.** `FindObject(widget, L"NameText")` in the
  POC scans the whole GUObjectArray and may collide with a same-named
  subobject elsewhere. Robust path: walk `WidgetTree.AllWidgets[]`
  property on the constructed widget. ~20 LOC extra; deferred until POC
  proves base path works.

---

## 10. Verdict

**FEASIBLE without VoidMod, UE4SS, or any other framework dependency.**

The path is:

1. Build a tiny UE 4.27 content project containing the widgets we want
   (`WBP_VOTVCOOP_Nameplate`, `WBP_VOTVCOOP_ChatBubble`,
   `WBP_VOTVCOOP_ServerBrowser`, ...).
2. Cook + pak as `votv-coop.pak`, mount root `/Game/Mods/VOTVCoop/`.
3. Ship the pak alongside `xinput1_3.dll` + `votv-coop.dll`; deploy
   script drops it under
   `Game_0.9.0n/.../VotV/Content/Paks/LogicMods/votv-coop/`.
4. UE4's startup pak scan auto-mounts it before our boot thread runs any
   reflection.
5. Our DLL calls `URyRuntimeObjectHelpers::LoadObject` (reflection,
   already-supported pattern) to resolve each widget `UClass*`, then
   `UGameplayStatics::SpawnObject(class, outer)` to instance -- the
   existing nameplate path with a different class pointer.
6. Graceful degradation: if pak missing, fall back to the programmatic
   widgets we already ship.

**No new AOB signatures. No new native function pointers. No UE4SS at
runtime.** The whole pak path piggybacks on infrastructure VOTV plugins
already expose to Blueprints, which we already drive via reflection.

The prior `MULTIPLAYER_UI.md` rejection ("ties us to UE4SS") was
**incorrect** -- it conflated `BPModLoader.lua`'s use of the pak with the
pak's loading itself. UE4 mounts the pak; UE4SS just runs Lua. Strike
that row from the comparison table; **the new conclusion is that BOTH
programmatic AND pak approaches are tool-agnostic and live alongside us
without coupling to UE4SS/VoidMod.**

**Recommendation:** stay programmatic for the two widgets already shipped
(nameplate, HUD feed -- RULE 2: no parallel paths), and pak the
*not-yet-built* polish surfaces (multiplayer menu, server browser, chat
bubble) where designer-authored UMG actually pays for itself. Land the
infrastructure (`engine::LoadCoopAssetClass`, deploy-pak step) in a small
self-contained increment before the first paked widget.

---

## Appendix A: file references

- CXX dump (reflection-callable APIs):
  - `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/PakLoader.hpp`
    -- UPakLoaderLibrary (Rama)
  - `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/RyRuntime.hpp`
    -- URyRuntimePakHelpers + URyRuntimeObjectHelpers
- Existing UMG reflection plumbing we reuse:
  - `src/votv-coop/src/ue_wrap/engine.cpp` lines 420-620 (SpawnObject,
    BuildTextWidget, SetTextOnBlock, AddWidgetToViewport)
  - `src/votv-coop/src/ue_wrap/call.cpp` (ParamFrame -- the marshaling
    primitive)
- Reference paks for header analysis:
  - `Game_0.9.0n/WindowsNoEditor/VotV/Content/Paks/VotV-WindowsNoEditor.pak`
    -- VOTV main pak, version 11, unencrypted
  - `reference/voidtogether-client-extracted/pak/VoidTogether.pak`
    -- third-party BP mod pak, version 11, unencrypted, Oodle, mount root
    `/Game/Mods/VoidTogether/`
- Prior design doc (to be updated post-this study):
  - `docs/MULTIPLAYER_UI.md` -- contains the original (now superseded)
    "BP mod ties us to UE4SS" rejection.
- BPModLoader reference (proves UE4 auto-mount):
  - `reference/RE-UE4SS/assets/Mods/BPModLoaderMod/Scripts/main.lua`
    -- never calls MountPakFile; iterates pre-mounted assets.

## Appendix B: pak header byte layout (UE 4.27 / version 11)

```
[footer @ file_size - 221 bytes]
+0x00 (1 B)   bEncryptedIndex flag (0 = plaintext index, 1 = encrypted)
+0x01 (4 B)   Magic 0x5A6F12E1 (little-endian)
+0x05 (4 B)   Version (11 for UE 4.27)
+0x09 (8 B)   IndexOffset (file offset of the index)
+0x11 (8 B)   IndexSize
+0x19 (20 B)  IndexHash (SHA1)
+0x2D (16 B)  EncryptionKeyGuid (all zero if no encryption)
+0x3D (160 B) CompressionMethods table (5 slots x 32-byte names; e.g. "Oodle\0...")
```

The bEncryptedIndex byte is the FIRST byte of the footer (immediately
before Magic). For both VOTV's main pak and VoidTogether's pak, this byte
is 0 -- so unsigned, unencrypted paks mount fine.
