# Building VOTV_MP

The mod ships as a single standalone DLL (`votv-coop.dll`) plus a proxy
loader (`xinput1_3.dll`). See [CLAUDE.md](CLAUDE.md) for the seven
architectural principles; this file is purely how to compile + deploy.

## Prerequisites

- **Windows 10 / 11** (build target is x64).
- **Visual Studio BuildTools** matching the MSVC vcpkg uses to build
  `protobuf:x64-windows-static`. vcpkg picks the **newest** installed VS
  by default. If you have multiple VS toolsets installed
  (e.g. VS 2019 + VS 2026 BuildTools side-by-side), this matters:
  - **Use the same VS version when configuring CMake** as vcpkg did
    when it built protobuf+abseil. If they diverge, you'll see
    unresolved `__std_*_trivial_N` symbols at link time (newer-stdlib
    vector-algorithm intrinsics missing from the older stdlib).
  - Tested 2026-05-28: **VS 18 BuildTools / MSVC 14.50.35717**. Pick the
    matching `-G "Visual Studio 18 2026"` generator below.
  - If you only have VS 2019 BuildTools, install protobuf via vcpkg
    with that toolset (set `VCPKG_VISUAL_STUDIO_PATH` env var to your
    VS 2019 install) so both halves use the same MSVC.
- **CMake 3.20** or newer.
- **Git** with submodule support.
- **vcpkg** — see one-time setup below.

## One-time setup

### 1. Clone the repo with submodules

```powershell
git clone --recursive https://github.com/<you>/VOTV_MP.git
cd VOTV_MP
```

Or if you already cloned without `--recursive`:

```powershell
git submodule update --init --recursive
```

Submodules pulled:
- `src/votv-coop/third_party/minhook` — hook engine (MIT).
- `src/votv-coop/third_party/GameNetworkingSockets` — wire layer (BSD-3),
  pinned to **v1.5.1**.
- `reference/RE-UE4SS`, `reference/mtasa-blue` — reference checkouts (not
  built; documentation/RE inputs only).

### 2. Install vcpkg (manifest mode)

vcpkg is a **build-time** dependency (not runtime — RULE №3 is preserved).
The build uses **manifest mode**: `src/votv-coop/vcpkg.json` pins the deps —
protobuf **3.21.12** (the last pre-abseil release) and openssl.

The protobuf pin is **load-bearing**, not arbitrary: with `USE_STEAMWEBRTC=ON`
the steamwebrtc / libwebrtc-lite ICE backend needs the vendored **2020 abseil**
(submodule). A *modern* protobuf (4.x/5.x) pulls its own modern vcpkg abseil,
and the two abseils' CMake target names (`absl::*`) collide at configure time
(`cannot create ALIAS target ... already exists`) — but **only** because we
`add_subdirectory(GameNetworkingSockets)` under our root. Pinning protobuf to
the last pre-abseil release means **one abseil exists**, so there is no
collision and `add_subdirectory(GNS)` stays as-is.

```powershell
# Install vcpkg at C:\vcpkg (standard location)
cd C:\
git clone https://github.com/microsoft/vcpkg.git
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics

# REQUIRED: unshallow so vcpkg can check out the historical protobuf 3.21.12
# port tree. A shallow clone fails the manifest install with
#   "git read-tree <sha> failed with exit code 128 ... cloned as a shallow
#    repository ... while loading protobuf@3.21.12".
git -C C:\vcpkg fetch --unshallow
```

No manual `vcpkg install` — manifest mode resolves + builds the pinned deps
(protobuf 3.21.12 + openssl, static-CRT) on the first configure. Expect
~15-20 minutes the first time; cached after.

## Configure

From the repo root, **one-time** configure that wires vcpkg + GNS:

```powershell
cmake -S src/votv-coop -B build/votv-coop -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static `
    -DVCPKG_MANIFEST_MODE=ON
```

The first configure installs the manifest deps (protobuf 3.21.12 + openssl)
into `build/votv-coop/vcpkg_installed/` — ~15-20 min once, cached after.

Notes on the flags:

- **`CMAKE_TOOLCHAIN_FILE`** — points CMake at vcpkg's
  `find_package(Protobuf)` shim. Required for GNS to find static protobuf.
- **`VCPKG_TARGET_TRIPLET=x64-windows-static`** — static CRT (`/MT`) match
  for our standalone DLL. RULE №3: no runtime dep on a vcpkg-installed
  `.dll`.
- **`VCPKG_MANIFEST_MODE=ON`** — uses the **top-level** manifest
  `src/votv-coop/vcpkg.json` (the `-S` dir), which pins protobuf to 3.21.12
  (see §2). vcpkg ignores GNS's own nested `vcpkg.json` (it's a subdir, not
  the `-S` manifest). Manifest install needs the unshallowed vcpkg from §2 to
  fetch the historical protobuf port; without it the configure fails with the
  `read-tree ... exit code 128` error.

Other generators by VS version (must match the MSVC vcpkg used to build
protobuf — see Prerequisites):

- VS 2019 BuildTools → `-G "Visual Studio 16 2019"`
- VS 2022 BuildTools → `-G "Visual Studio 17 2022"`
- VS 2026 (18) BuildTools → `-G "Visual Studio 18 2026"` (default 2026)

## Build

After the one-time configure, the day-to-day build is:

```powershell
cmake --build build/votv-coop --config Release
```

Output:

- `build/votv-coop/Release/votv-coop.dll` — the mod payload.
- `build/votv-coop/Release/xinput1_3.dll` — the proxy loader.

Expected first clean build: 5-10 min (GNS is ~50k LOC; the protobuf-
generated `.pb.cc` files also compile). Incremental: under a minute when
only our sources change.

## Deploy to the three game copies

```powershell
.\tools\deploy-all.ps1
```

This copies the new DLLs into the host / client / dev copies of
`Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64\`. The deploy is
idempotent.

For the autonomous LAN smoke (per the pre-deploy checklist in CLAUDE.md):

```powershell
.\mp_host_game.bat    # in one window
.\mp_client_connect.bat   # in another
```

Both .bat files set the OBS-watched window titles ("VotV (Host)" /
"VotV (Client)").

## Troubleshooting

### `find_package(Protobuf)` fails

Re-run `vcpkg install protobuf:x64-windows-static`. Confirm
`C:\vcpkg\installed\x64-windows-static\lib\libprotobuf.lib` exists.

### `error C2039: "c_str": is not a member of std::basic_string_view`

You're building against GNS v1.4.0 or older with modern protobuf. Confirm
the submodule is at v1.5.1: `cd src/votv-coop/third_party/GameNetworkingSockets && git log --oneline -1`
should show `fa489fd`.

### `versions/baseline.json` not found during configure

vcpkg is in manifest mode and trying to fetch a baseline that's not in
your local vcpkg history. Add `-DVCPKG_MANIFEST_MODE=OFF` to the configure
line (see above).

### DLL loads but no GNS symbols in process

PR-1 deliberately does NOT call any GNS API at runtime (the smoke file at
`src/coop/net/gns_smoke.cpp` only takes one symbol address to force the
link). PR-2 wires GNS into the live `Session`. This is expected behaviour
until then.

## Cleaning

Reconfigure from scratch:

```powershell
Remove-Item -Recurse -Force build/votv-coop
# then re-run the configure command above
```

Drop everything (vcpkg, sandbox, build):

```powershell
Remove-Item -Recurse -Force build/votv-coop
# C:\vcpkg can be deleted too; nothing else depends on it.
```
