# Publishing Multivoid to Thunderstore — the procedure

**What this doc owns:** the repeatable *how* — what goes in the zip, what the manifest must say, how
the first upload and every update work, what the platform will and will not let us undo, and the
pre-flight checklist. Written 2026-08-25 from the official wiki (11 pages, listed at the bottom)
plus the measurements in `docs/UE4SS_ARC.md` §7.

**What this doc does NOT own, so it is not duplicated here:**

| question | owner |
|---|---|
| Where each file lands after install, and the four traps | `UE4SS_ARC` §7.2a — **authoritative**, measured |
| What real VOTV packages ship, and our ABI/CRT vs theirs | `UE4SS_ARC` §7.2b |
| `version_number` mapping (`<game-major>.<game-minor>.<build>`) | `UE4SS_ARC` §7.3 — DECIDED |
| One zip / manual assembly / what the pak costs | `UE4SS_ARC` §7.4c, §7.9, §7.7c |
| Player-facing install prose | `docs/INSTALL.md` (single owner) |
| The GitHub release ritual | `docs/RELEASE.md` |

Status: **PROCEDURE, NOT YET EXECUTED.** Multivoid has never been uploaded. Nothing below has been
run end to end; it is the wiki's stated rules plus our measured shape. The first upload is also the
first test of this document.

---

## 1. Preconditions — what must be true before the first upload

> **THE UPLOAD HAPPENED — 2026-09-01T18:06:11Z. This section is now HISTORY, kept because a
> second package (or a game-target move) walks the same list.** `[V]` measured 2026-09-02 off
> `api/experimental/package/Pelmentor/Multivoid/`: `Pelmentor/Multivoid` v`0.9.150`, not
> deprecated, 22 downloads. All seven preconditions are DONE. Two package-level fields are
> **still empty and neither can be set from this repo** — see §3a: `donation_link` (the support
> button) and `categories` (discoverability). Both are website settings, both are editable
> forever, and both are the user's to set.

| # | Precondition | State as of 2026-08-25 |
|---|---|---|
| 1 | A Thunderstore **Team** exists. The team name becomes the `<Author>` half of `<Author>-Multivoid`, which is load-bearing for the pak path (`UE4SS_ARC` §7.2a trap 4). | **DONE 2026-08-29 (USER): the team is `Pelmentor`** — created that day, SUPERSEDING the 2026-08-26 pick of `Multivoid`. So the package is `Pelmentor-Multivoid`, the pak path is `shimloader/pak/Pelmentor-Multivoid/`, and the zip is `Pelmentor-Multivoid-<version>.zip`. **WHY it changed, and why the change was free:** the team IS the namespace — `manifest.json` carries no author field at all (§3) — so `Multivoid-Multivoid` displayed the author as a project rather than a person, which the user saw in r2modman on a LOCAL import (it derives the author from the zip filename) and rejected. Nothing had been published, so §5's irreversibility had not yet bitten; **after the first upload it does**, and neither half can move without creating a SECOND package. `dependencies` is unaffected — it names shimloader's team, not ours. The pak path needed no code change: `skin_registry.cpp:156-158` already scans every `LogicMods` subdirectory *because* the managed lane lands paks in a directory we do not name (precondition 5). |
| 2 | A **service account** + API token, if publishing from CI (`TCLI_AUTH_TOKEN`). | **NOT DONE, and still optional — but for a different reason than it says in §7.9.** That entry made it optional because we assembled the zip by hand; since 2026-09-01 CI assembles and verifies it on every push and `release-core.yml` publishes it as the GitHub release asset (`73e73dd6`, `4c5e92ce`). What is still manual is the **Thunderstore upload itself** — a GitHub release is not a Thunderstore publish, and nothing in this repo talks to Thunderstore's API. So the token is needed only if we ever want that last hop automated |
| 3 | `icon.png`, **exactly 256x256** | **DONE** — `assets/branding/icon.png`, generated from `icon-512.png`. **The art was replaced 2026-09-01** (user's third revision) and re-generated; `[V]` 256x256 with transparent corners, and a real `package.ps1` run staged it with the zip's `icon.png` byte-identical to the tree's (`sha256 7149ff3d...`) — the gate at `package.ps1:69-75` re-MEASURES the PNG rather than trusting the filename, so that run is the proof, not the eyeball. (Per item 4: do not reach for that zip by name — re-run the packager and take the name it prints.) Note §5's immutability: whatever is on disk at package time is what ships **forever** under that version number — so the art is a pre-flight check, not a post-upload fix. |
| 4 | A UE4SS-lane build actually released | **DONE 2026-09-01** — `v0.9.0n-b150-dev` is published, prerelease, one asset `Pelmentor-Multivoid-0.9.150.zip` (11,060,341 B, `sha256 dd21ae37…b53ea5b8`), assembled on the runner by `package.ps1` from the tagged cacheless rebuild. This was the LAST precondition and it was the only one gating the upload. **The file to upload is the one on the release page** — not a local build: the published zip carries the CI bytes, and `[V]` an entry-by-entry diff against the locally approved zip showed 8 files byte-identical, 3 text-identical (LF here / CRLF on the runner), and only `mod/dlls/main.dll` differing, which is the designed lane. (The older warning against reaching for a named local file stands for any OTHER purpose: the build number moves several times a day.) |
| 5 | `skin_registry` walks `LogicMods/` subdirectories | **DONE 2026-08-29** (`aaf695c4`) — `PakDirs()` scans EVERY LogicMods subdirectory (multivoid/ first, stems deduped; the TOP level stays excluded — foreign BP mods like DebugMod.pak must not list as skins). The zip now carries `pak/` -- since the same day, ONE bundle `scientists.pak` holding all four starter scientists (user decision) plus the four preview tiles by member name; the bundle->members map is `kSkinBundles` in `skin_registry.cpp`, because the loader keys on the assets' internal `/Game/Mods/VOTVCoop/<name>` paths and never on the pak filename. The manual-lane step is in INSTALL.md |
| 6 | The negative control: our own zip imported locally into r2modman and the profile tree diffed against §7.2a's prediction | **DONE 2026-08-29 for LAYOUT** — the user imported `Pelmentor-Multivoid-0.9.145.zip` through Settings → Import local mod and the tree matched the prediction exactly: `mod/` → `shimloader\mod\Pelmentor-Multivoid\` (r2modman FLATTENS it, so `main.dll` sits at that folder's root, not under `dlls\`), `pak/` → `shimloader\pak\Pelmentor-Multivoid\` with `scientists.pak` + the four tiles, and the mod booted (`boot: Multivoid 0.9.0n b145`, HEALTH PASS, hooks installed). What this does NOT cover: the session itself never came up in that run — see `memory/project-r2modman-field-test-open-defects-2026-08-29.md`; the cause of the visible half is the master's build gate, not the package. **THE SECOND TEST HAPPENED 2026-09-01 and the user's verdict was "Протестил, достойный релиз".** What is `[V]` from here: a **b150** peer hosted a live lobby on the PRODUCTION master during that window (`97c4dfb9aaa074f4`, proto 150, conn `p2p`, 1/4, read off `/v1/lobbies`) — so on b150 the session comes up, registers and lists, which is exactly what the 2026-08-29 import could not reach. **What I did NOT measure is the install ROUTE** — whether that peer was the managed r2modman import or a dev-deployed rig. So treat this row as: the BUILD is user-approved and session-proven; the managed-lane *layout* remains proven only by the 2026-08-29 import. The obstacles named on 2026-08-31 are all gone (`COOP_MAX_BUILD` retired in `24418b66`; master + relay cut over, `sig_gate` PASS 14/14 in production, `hostIdentity` on DIRECT `/v1/join`). Sequencing now lives in `docs/RELEASE.md`'s "What is still owed" list. |
| 7 | The game is in the ecosystem schema | **DONE, not by us** — `voices-of-the-void` is listed with `packageLoader: shimloader`; no PR to `ecosystem-schema` is needed. Adding a game requires "pre-existing mod developer interest" and a CLI-generated PR — irrelevant to us, recorded so it is not re-asked |

## 2. The zip

Root files are **required and case-sensitive** — `manifest.json`, `icon.png`, `README.md`. Everything
else is routed by the per-game rules. Our tree, per `UE4SS_ARC` §7.2a:

```
manifest.json          icon.png (EXACTLY 256x256 PNG)   README.md   CHANGELOG.md
mod\enabled.txt
mod\dlls\main.dll
pak\scientists.pak     (+ the <name>.png preview tiles)
```

- **Icon:** 256x256 PNG. Transparency is supported. An animated PNG is technically valid but only the
  first frame renders in some contexts — do not ship one.
- **README.md:** UTF-8, markdown "closely but not exactly" GitHub-flavoured. Thunderstore has a
  preview tool; use it rather than assuming GitHub's renderer. This file **is** the package page.

  **ITS SOURCE IS `tools/release/README_thunderstore.md` — a DEDICATED store page, NOT the repo
  README** (USER 2026-08-30; the one-day-old repo-README-verbatim scheme is retired). The store
  page is a short player-facing text without the repo README's developer sections or the
  author's note, and it carries ABSOLUTE links only — the repo README's relative links (docs/,
  src/) render broken on the store page. `package.ps1` stages it into the zip root as
  `README.md` and FAILS CLOSED if the file is missing or does not mention the current game
  target (a hand-written pair in a page nobody regenerates is how version strings rot). The
  Boosty support badge lives in BOTH this file and the repo README — a support-rail change
  touches both, or the two pages disagree about how to give. **RESOLVED 2026-09-01/02, and the
  two halves resolved in OPPOSITE directions on purpose.** The store README ships with **no
  badge** (`415d2f67`): a published version is immutable, so a badge frozen into v0.9.150 could
  never be changed, while `donation_link` buys the same button and stays editable forever. The
  repo README badge, the Support row and `.github/FUNDING.yml` are **restored** (`d9b6ab9a`,
  after `7ebc2554` pulled them and `c18003aa` kept them pulled) — that surface is mutable, so
  it costs nothing to carry. The old note said *"the badge HERE stays because this page is not
  live until the first upload"*; the page is live now and the badge is not on it.

  **Consequence for the FIRST upload, and it is one-shot:** a published version is IMMUTABLE
  (§5), so a README change costs a whole new version number. Anything that belongs on the store
  page — the support link included — must be in `README_thunderstore.md` *before* `package.ps1`
  runs for the first published version, or it waits for the next one.
- **CHANGELOG.md** is optional (3 of the 5 field packages ship one).
- **Size ceiling: 5 242 880 000 bytes (~5 GB).** Our one-zip package is ~32 MB at the outside
  (§7.7c). Not a constraint.
- **The wiki states the routing rule in general terms and it matches what we measured:** *"The
  Thunderstore Mod Manager will empty the contents of folders in your package's .zip file unless they
  are inside specifically named folders."* For VOTV the named folders are `mod`, `pak`, `cfg`,
  `overlay` — which is exactly why a root-level `dlls/` loses its folder and never loads.

## 3. `manifest.json`

Every constraint, from the wiki, with our value beside it:

| field | rule | ours |
|---|---|---|
| `name` | no spaces; **only `a-z A-Z 0-9 _`**; max 128 chars | `Multivoid` — legal |
| `version_number` | semver `Major.Minor.Patch`, **no suffixes**; each part is a **whole number**, so `1.0.10` is nine patches above `1.0.1` | `<game-major>.<game-minor>.<kProtocolVersion>` per §7.3, e.g. `0.9.141` |
| `description` | **max 250 characters** | must be written; it is also the gallery subtitle |
| `dependencies` | array of `{team}-{package}-{version}` | `["Thunderstore-unreal_shimloader-1.1.7"]` — what all four field mods with code declare |
| `website_url` | optional, **but the key must exist; use `""` if unused** | `https://multivoid.dev` |

`author` is **not** a required field — 3 of 5 field packages omit it entirely. The namespace comes
from the uploading Team, not from the file.

**GENERATE it, never hand-edit it** (`UE4SS_ARC` §7.3, HARD REQUIREMENT). A hand-kept version string
that rots unbumped is the exact failure that got mod semver deleted in 2026-07-19; re-introducing a
typed `version_number` recreates it one layer out. Emit at package time from `VOTVCOOP_GAME_TARGET`
+ `kProtocolVersion`, and fail closed if either parse misses.

## 3a. The support link — allowed, first-class, and only half of it is immutable

**Asked by the user 2026-08-31 ("а так можно?"), answered by measurement, not by assumption.**

**It is allowed, and Thunderstore has a built-in field for it.** `[V]` every package object in the
public API (`https://thunderstore.io/c/voices-of-the-void/api/v1/package/`) carries a
**`donation_link`** key, beside `owner` / `is_deprecated` / `categories`. It is **not** in
`manifest.json` (§3 lists every field) and **not** on the version object — so it is set on the
website at PACKAGE level, which means, unlike anything in §5, **it can be changed later without
burning a version number.**

`[V]` **Measured across the live VOTV catalog, 2026-08-31: 51 of 188 packages publish one** —
ko-fi 42, patreon 5, **boosty.to 1**, plus three one-offs. Including `ebkr-r2modman` itself, the
mod manager, which links a charity.

`[V]` **Boosty specifically already has a precedent in this exact community**:
`Antoha256M-Manual_Russian_Translation` (a Russian manual translation) links
`boosty.to/antoha256m` — live, **not deprecated**, 4,973 downloads, rating 11. So the platform
is not merely tolerated in the abstract; it is in use, in this community, unflagged.

**The one rule that touches this** is the spam clause in the global rules — *"packages that exist
primarily to advertise an outside platform or service"* are not allowed. That is aimed at a package
whose REASON is the advert; a working mod with a support link is not it. The global rules say
**nothing** about donations, paywalls, paid mods, or link shorteners
(`wiki.thunderstore.io/moderation/global-rules`, re-read 2026-08-31 — note the old
`thunderstore.io/wiki/...` path now 404s).

**So the link goes in two places, and they cost differently:**

| surface | mutable after publish? | consequence |
|---|---|---|
| package `donation_link` (website setting) | **YES** | change it any time; a wrong or dead URL is a five-second fix |
| the badge in `README_thunderstore.md` | **NO** — baked into the version (§5) | a change costs a whole new version number |

The field is what the site renders as a support button; the badge is what a reader sees in the page
text. Decide the README half *before* `package.ps1` runs and treat the field as the recoverable one.

~~**Status, 2026-08-31 — none of this is live**~~ **— SUPERSEDED 2026-09-01/02. The rails are up
and the store field is not.** History, so the reversal is not re-litigated: the repo badge was
pulled (`7ebc2554`), restored on an inference (`1aca131b`), pulled again on the user's explicit
*"No dont restore"* (`c18003aa`), and finally restored on their explicit instruction once the
listing existed (`d9b6ab9a`). *The page existing and the buttons going live were always two
different decisions, and only the user made the second one.* The package README shipped WITHOUT
the badge (`415d2f67`) because a published version is IMMUTABLE and `donation_link` buys the same
button (the one r2modman renders beside Download, `[V]` seen on `Flyingcoyote-VoidFax`) while
staying editable forever.

**`[V]` THE FIELD IS STILL EMPTY — measured 2026-09-02, not assumed.**
`api/experimental/package/Pelmentor/Multivoid/` returns `donation_link` empty and **`categories`
empty too**. Neither can be set from this repo: both are website settings on the package, both
survive every future version, and both are the user's to click. `categories` was never on this
checklist and should have been — an uncategorised package is missing from every filtered browse
in the manager, which is how most people find anything.

**A measurement trap for the next upload, since it cost a confused minute here.** Right after
publishing, the per-package endpoint (`api/experimental/package/<team>/<name>/`) already served
the package while the community catalog (`c/voices-of-the-void/api/v1/package/`) did **not** —
`[V]` absent at 18:5x, present ~an hour later (189 packages, up from the 188 this doc measured on
2026-08-31). A freshly published package missing from the catalog endpoint is indexing lag, not a
failed upload; query the per-package endpoint to tell them apart.

## 4. First upload

1. Zip the tree in §2. **The files must be at the zip root — not nested inside an extra folder.**
2. Go to `thunderstore.io/package/create/`, pick the **Team**, upload.
3. Choose **categories**. VOTV's community offers: `mods`, `modpacks`, `tools`, `libraries`, `misc`,
   `audio`, `items`, `language`, `tweaks`, `console`, `kerfur`, `signals`, `crafts`, `placeables`,
   `nsfw`. **There is no multiplayer/co-op category** — `mods` is the section that matters
   (the `mods` section excludes anything categorised `modpacks`).
4. Leave the NSFW flag off.

## 5. Updating — the rules that cannot be undone

This is the section to read before the first upload, not after.

- **A version is IMMUTABLE.** *"Once a version is successfully uploaded, it can no longer be
  edited."* You cannot overwrite it, cannot fix a typo in it, and cannot reuse the number.
- **Fixing the README requires a new version.** The page text is part of the version.
- **`name` and Team must be identical**, or the upload creates a **new package** instead of updating
  the existing one. That is the one mistake that produces a duplicate listing.
- `version_number` must be strictly higher. Our mapping gives this for free —
  `kProtocolVersion` never resets and only increases (§7.3).
- **Categories may be left blank on an update** and the original selection is preserved.

**Consequence for us, stated plainly:** because a version is permanent and `kProtocolVersion` moves
on every wire change including security-only ones (§7.3a), a botched upload is not repairable — it is
survivable only by publishing the next build number. Run the §1 item 6 local-import control first.

## 6. After upload — why it may not appear

Four documented reasons, in the order to check them:

1. **Propagation.** Several minutes before it shows on the community page or in search. The direct
   package link works immediately.
2. **Category.** A mod categorised `modpacks` shows in the Modpacks section, not Mods.
3. **Mod-manager cache — up to THREE HOURS**, and different users get it at different times. The
   wiki's own advice for testing is to **import the zip locally**, which is the same control §1
   item 6 books.
4. **Rejection.** Packages go live automatically, but an automated system flags some into a
   **per-community Review Queue** where a Community Moderator approves or rejects with a note. A
   rejected package is **invisible to everyone except moderators and the uploader** — it does not
   error, it just is not there. *"Packages are often rejected due to accidentally uploading files
   from another mod."* Recourse is the rejected-uploads forum on the Thunderstore Discord; VOTV's
   moderators are reachable via the community Discord (`discord.gg/WKBvqu4tjV`).

## 6b. When a player says "installed it, doesn't work" — triage order

Half of these reports will not be our bug. The manager's own documented causes, cheapest first, plus
the one VOTV-specific cause that outranks all of them:

1. **`xinput1_3.dll` beside the exe — check this FIRST, and it is ours.** `unreal_shimloader`
   **Rust-panics by design** on seeing that filename (its guard targets 2023-era UE4SS and hits our
   retired proxy purely by name). The game process comes up windowless and idle, **nothing loads at
   all**, and the only diagnostic is `Win64/shimloader-log.txt`, which names the exact file and the
   removal steps. A player upgrading from a standalone Multivoid install into the manager lane hits
   this every time. Full measurement: `[[lesson-shimloader-owns-the-xinput-error-surface]]`.
2. **No mod loader in the profile** — `unreal_shimloader` must be installed in *that* profile.
3. **Launch parameters** must be empty in **both** Steam's `LAUNCH OPTIONS` and the manager's
   `Set launch parameters`.
4. **Wrong game folder / Steam folder** in the manager settings; also, the manager's *data* folder
   must not sit inside the Steam or game directory.
5. **Anti-virus quarantine** of files in the data folder or the game folder. Shimloader binaries are
   a known Defender false positive (upstream says so in its own README).
6. **Leftover manual installs** in the game directory — remove them.
7. **Download/SSL failures**: Settings -> "Toggle preferred Thunderstore CDN", or a different
   connection. Usually transient.

For a support thread, ask the player for the manager's **"Copy troubleshooting information to
clipboard"** output (Settings) — plus, for us, `multivoid.log` and `shimloader-log.txt` from
`VotV\Binaries\Win64\`.

## 7. The moderation rules that actually bite this package

Most global rules are irrelevant to us (no malware, no NSFW, no spam, no harassment). **Two are
not**, and both are aimed at the same file:

> *"Do not reupload packages or assets by other authors unless you have permission to redistribute
> them or are following their licensing."*
>
> *"Copyright laws and code licensing must be followed where applicable. **Do not distribute game
> files** such as `Assembly-CSharp.dll`, unless given explicit permission by the game's developers."*

`scientists.pak` has **two independent exposures** to those lines:

1. the skin meshes are **Valve-derived** (Half-Life scientist models) — the first rule;
2. the cooked template the conversion chain builds against
   (`kerfurOmega_KelSkin`, `ue_cook.py:27`) was **extracted from VOTV's own paks** — the second rule
   names game files specifically.

**The decision that the skins ship is SETTLED (`UE4SS_ARC` §7.6, USER 2026-08-23) and this section
does not re-open it.**

### 7a. What the community actually ships — MEASURED 2026-08-25, and it moderates the above

The user's answer to the paragraph above was that VOTV's Thunderstore already carries skins from
other games, so this is fine. **Checked against the live catalog** (`thunderstore.io/c/voices-of-the-void/api/v1/package/`,
185 packages), and the substance holds: cross-property **asset replacement is normal here and is
neither hidden nor deprecated**. Live, non-deprecated examples:

| package | what it replaces with |
|---|---|
| `Hirokhai-MinecraftBeehive` | *"Replaces the beehive and beebox with **minecraft**"* |
| `forder-Kerfur_Kurobara` | a commissioned character, *"rigged to the game's own skeleton"* |
| `Yojimo-Kerfuro_Snickers` | *"Replaces Kerfur-o with Lenyavok's Snickers"* (another creator's OC) |
| `AmariMakes-NSFW_Loona_3d_prints` | Helluva Boss character models — NSFW-flagged **and "MANUAL DOWNLOAD REQUIRED"** |

**One correction to the user's framing, on the record because it is the leg that matters for us:**
the catalog's Half-Life presence is **code, not assets** — `Moddy-PBMovement` ports Project Borealis'
MIT movement code with attribution, and `blueprintwastaken-HL2AHop` adds a mechanic. Neither ships
Valve meshes. Searching package **names and descriptions** finds no bundled-HL-asset precedent; that
is a limit of the search, not proof of absence (a package can ship assets without saying so).

**So the honest reading, which is what this section should have said the first time:**

- **Leg 1 (third-party character assets) has strong live precedent** and the written rule is plainly
  not enforced against it in this community. Treat it as accepted practice.
- **Leg 2 (the cooked template extracted from VOTV's own paks) has no observed precedent either
  way**, and it is the leg the rule names *explicitly* (*"do not distribute game files"*). It is also
  the quieter one: it is not what a moderator would notice, and it is not what a takedown would
  usually be about.
- The one behaviour worth copying regardless is `AmariMakes`': that author **kept the third-party
  models out of the Thunderstore zip** and made them a manual download. We are not doing that (one
  zip, §7.4c) — noted only so the option is on the record rather than re-invented.

**Net: downgrade this from "a risk to weigh" to "a known, accepted community practice with one
un-precedented leg."** The failure mode if it ever does bite is still the §6 silent rejection, and
§8 still means we could not delete the package ourselves — those two facts are unchanged and are why
the section stays.

### 7b. Someone already holds a VOTV coop listing — MEASURED 2026-08-25

`migabyte-VotVCoop` (`thunderstore.io/c/voices-of-the-void/p/migabyte/VotVCoop/`) — *"Co-op
multiplayer mod for Voices of the Void. Not publicly functional at the moment"* — v0.1.1, last
updated **2026-03-30**, categories `Tweaks/Mods/Tools`, and **DEPRECATED**. Recorded because it is
the first thing a VOTV player searching "coop" finds, and because nobody on this project knew it
existed. It does **not** collide with us: the package name is `VotVCoop` under team `migabyte`, ours
is `Multivoid` under our own team, and Thunderstore keys on `<team>-<name>`. No action; context only.

## 8. Removal — we cannot delete our own package

- **An author cannot delete a package.** The only self-service action is **deprecate**, via the
  "Manage Package" button.
- **Deprecation** marks it as no-longer-to-be-used. It stays discoverable and downloadable as a
  dependency, existing installs keep working, and the status **clears automatically when a new
  version is published** — so it is reversible.
- **Deletion is administrator-only**, "usually reserved for packages that contain illegal content or
  other serious issues", and requires contacting support.
- A **rejected** package is not removed from anyone's existing install; it just stops appearing in
  the manager's list.

## 9. Modpacks and profiles — not our lane, recorded so it is not re-asked

A **modpack** is a package in the `modpacks` category whose `manifest.json` is mostly a
`dependencies` list — configs, no code. A **profile** is a shareable code generated inside the mod
manager (Share button) that pulls a mod list + configs. Neither is how Multivoid ships; both are
relevant only if we ever want a "recommended co-op setup" bundle, which is not scoped.

## 10. Pre-flight checklist

Run top to bottom. Nothing here is satisfied by "the zip built".

1. §1 preconditions 1, 4, 5 are DONE. (2 is optional; 3 and 7 are already done.)
2. `manifest.json` was **generated**, and `version_number` equals `<game-major>.<game-minor>.<kProtocolVersion>`
   read from the tree, not typed. **Satisfied structurally since 2026-08-26** by
   `tools/release/package.ps1` + `ledger_lib.ps1`'s `New-PackageManifest` / `ConvertTo-PackageVersion`,
   which read both halves through the tree's one parser each and throw labeled `UNREADABLE` rather
   than defaulting. Items 3 and 4 are likewise machine-checked now (`Get-PngDimensions` re-measures
   the icon; `Test-PackageZip` asserts the tree, and its drill has 14 arms). **Item 5 is NOT
   automated and item 6's managed half has NOT run** — see `UE4SS_ARC` §7.8.
3. `icon.png` is byte-for-byte 256x256 (re-measure it; do not trust the filename).
4. The zip's root holds `manifest.json` + `icon.png` + `README.md` with **no wrapping folder**, and
   `mod\dlls\main.dll` is at `mod\dlls\`, not at the root.
5. `main.dll` came from the **tagged run's CI artifact**, never a local build (`UE4SS_ARC` §7.9 —
   this project has already shipped wrong bytes from a payload picked by mtime).
6. The **local-import control** ran: r2modman "Import local mod", and the resulting profile tree
   matches `UE4SS_ARC` §7.2a's prediction — `shimloader/mod/<Author>-Multivoid/dlls/main.dll`
   exists, and `shimloader/pak/<Author>-Multivoid/scientists.pak` exists.
7. The skin browser lists the bundled skins **in that imported profile**, not only in a dev install
   (this is what proves §7.7 actually landed).
8. `README.md` previewed through Thunderstore's own markdown preview, not assumed from GitHub.
9. `description` is under 250 characters.

Only after 1-9: upload. Then re-check §6 before concluding anything is wrong.

---

Sources — Thunderstore Wiki, read 2026-08-25: `mods/creating-a-package`, `mods/updating-a-package`,
`mods/packaging-your-mods`, `mods/mod-not-visible`, `sharing-your-mods/modpacks-and-profiles`,
`mod-manager/game-wont-launch-modded`, `mod-manager/common-issues`, `ecosystem/adding-a-new-game`,
`moderation/global-rules`, `moderation/removing-a-package`, `moderation/community-moderators`.
Package-shape measurements: `docs/UE4SS_ARC.md` §7.2a / §7.2b (five real VOTV packages, the live
ecosystem schema, and r2modman's own rule engine + test spec).
