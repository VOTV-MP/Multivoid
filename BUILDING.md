# Building Multivoid

The mod ships as a single standalone DLL plus a proxy loader
(`xinput1_3.dll`). See [CLAUDE.md](CLAUDE.md) for the seven architectural
principles; this file is purely how to compile + deploy.

The payload DLL's **filename is load-bearing**: it is
`multivoid-<game-target>-<build>.dll` (the Paper-Minecraft pair — e.g.
`multivoid-0.9.0n-125.dll`). The xinput proxy scans for `multivoid-*.dll`,
loads the **highest build number** it finds, and pops an in-game
"MOD INSTALL PROBLEM" dialog on duplicates. Do not rename the output.

Both halves of the name come out of the source at configure time — the game
target from `VOTVCOOP_GAME_TARGET` in `src/votv-coop/CMakeLists.txt`, the
build number parsed out of `kProtocolVersion` in
`include/coop/net/protocol.h`. **A protocol bump therefore renames the
artifact**, and the configure re-runs automatically when `protocol.h`
changes.

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
git clone --recursive https://github.com/VOTV-MP/Multivoid.git
cd Multivoid
```

Or if you already cloned without `--recursive`:

```powershell
git submodule update --init --recursive
```

Submodules that are **compiled into the DLL** — all four are required; a
missing one fails the CMake configure, not the link:

- `src/votv-coop/third_party/minhook` — hook engine (MIT), v1.3.4.
- `src/votv-coop/third_party/GameNetworkingSockets` — wire layer (BSD-3),
  pinned to **v1.5.1** (`fa489fd`).
- `src/votv-coop/third_party/imgui` — Dear ImGui (MIT), pinned **v1.91.5**,
  for the dev menu / server browser. Our own copy, never UE4SS's (RULE 3).
- `src/votv-coop/third_party/opus` — libopus (BSD-3), pinned **v1.5.2**, the
  voice codec.

Reference-only checkouts (never built; documentation / RE inputs):
`reference/RE-UE4SS`, `reference/mtasa-blue`. They are large; if you only
want to build, `git submodule update --init --recursive
src/votv-coop/third_party` skips them.

Two more third-party deps are **vendored in-tree**, so they need no
submodule action: `third_party/freetype` (2.13.3, FTL) and
`third_party/miniaudio` (0.11.22, single header).

**The `--recursive` is not optional and it is not cheap.** We build GNS with
`USE_STEAMWEBRTC=ON` (the ICE / NAT-punch backend), and GNS's
`src/external/steamwebrtc` hard-fails the configure unless GNS's OWN nested
submodules are present — chiefly `src/external/webrtc` from
`webrtc.googlesource.com`, which is **~263 MB checked out** and slow to
fetch, plus `src/external/abseil` (~39 MB). Budget time for that clone.

### 2. Install vcpkg (manifest mode)

vcpkg is a **build-time** dependency (not runtime — RULE №3 is preserved).
The build uses **manifest mode**: `src/votv-coop/vcpkg.json` pins the deps —
protobuf **3.21.12** (the last pre-abseil release), openssl, and
nlohmann-json (header-only; parses the master-server JSON in
`coop/net/lobby_*`).

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
(protobuf 3.21.12, openssl, nlohmann-json; static-CRT) on the first
configure. Expect ~15-20 minutes the first time; cached after.

## Configure

From the repo root, **one-time** configure that wires vcpkg + GNS:

```powershell
cmake -S src/votv-coop -B build/votv-coop -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static `
    -DVCPKG_MANIFEST_MODE=ON
```

The first configure installs the manifest deps into
`build/votv-coop/vcpkg_installed/` — ~15-20 min once, cached after.

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

- `build/votv-coop/Release/multivoid-<game>-<build>.dll` — the mod payload
  (e.g. `multivoid-0.9.0n-125.dll`; see the note at the top of this file —
  the filename is what the proxy scans for).
- `build/votv-coop/Release/xinput1_3.dll` — the proxy loader.

Expected first clean build: 5-10 min (GNS is ~50k LOC; the protobuf-
generated `.pb.cc` files also compile). Incremental: under a minute when
only our sources change.

## Deploy to the game copies

```powershell
.\tools\deploy-all.ps1
```

This copies the proxy + payload DLLs into **four** local game copies —
`Game_0.9.0n_HOST` (HOST), `_CLIENT_1` (CLIENT), `_CLIENT_2` (CLIENT2), and
`_CLIENT_3` (DEV, the autonomous-test copy) — each at
`...\WindowsNoEditor\VotV\Binaries\Win64\`. It also ships the client-puppet
mesh pak into `...\Content\Paks\LogicMods\multivoid\`. The deploy is
idempotent (identical bytes are skipped).

That four-copy layout is this repo's own test rig. If you are just building
for yourself, copy `xinput1_3.dll` + `multivoid-*.dll` next to `VotV-Win64-Shipping.exe`
in your own install instead.

For the autonomous LAN smoke (per the pre-deploy checklist in CLAUDE.md):

```powershell
.\mp_host_game.bat    # in one window
.\mp_client_connect.bat   # in another
```

Both .bat files set the OBS-watched window titles ("VotV (Host)" /
"VotV (Client)").

## Troubleshooting

### `fatal: No url found for submodule path '.../opus' in .gitmodules`

You are on a clone from before 2026-07-24, when the `opus` gitlink was
committed without its `.gitmodules` entry — `--recursive` skipped it
silently and the configure then died on the empty directory. Pull the
current `main` and re-run `git submodule update --init --recursive`. To fix
an existing checkout in place without pulling:

```powershell
git clone --depth 1 -b v1.5.2 https://github.com/xiph/opus.git src/votv-coop/third_party/opus
```

### `Submodule .../webrtc is not checked out` / steamwebrtc configure error

GNS's own nested submodules were not fetched. `--recursive` covers them:

```powershell
git submodule update --init --recursive src/votv-coop/third_party/GameNetworkingSockets
```

Expect a slow clone — webrtc is ~263 MB checked out.

### `find_package(Protobuf)` fails

The manifest install did not complete. Delete `build/votv-coop` and
re-run the configure; watch for the vcpkg errors it prints on the way past
(the `read-tree ... exit code 128` shallow-clone failure below is the most
common cause). Confirm
`build/votv-coop/vcpkg_installed/x64-windows-static/lib/libprotobuf.lib`
exists afterwards. Do **not** `vcpkg install` by hand — this build is
manifest-mode, and a classic-mode install lands in a directory the
toolchain will not read.

### `error C2039: "c_str": is not a member of std::basic_string_view`

You're building against GNS v1.4.0 or older with modern protobuf. Confirm
the submodule is at v1.5.1: `cd src/votv-coop/third_party/GameNetworkingSockets && git log --oneline -1`
should show `fa489fd`.

### `versions/baseline.json` not found, or `git read-tree ... exit code 128`

vcpkg cannot reach the pinned `builtin-baseline` in its own history —
almost always because vcpkg was cloned shallow. Unshallow it (this is the
same step as §2, worth re-running):

```powershell
git -C C:\vcpkg fetch --unshallow
```

Turning manifest mode **off** is not the fix: the manifest is what pins
protobuf to 3.21.12, and without that pin the configure fails later on the
duplicate-abseil `ALIAS target` collision described in §2.

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
