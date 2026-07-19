#!/usr/bin/env python3
"""rng_census_analyze.py -- T1 fork-call adjudication over rng_roll_census logs.

Parses both peers' multivoid logs for the probe v9 [RNG-CENSUS] records and
produces the PRE-REGISTERED adjudication table (docs/COOP_RNG_AUTHORITY.md,
"T1 PRE-REGISTRATION"): one row per pre-registered T1-1 shared-world spawner,
vocabulary live / armed / confirmed-starved / DONE-suppressed / CONDITIONAL,
plus the callable-check arithmetic (>= 2/3 of 16 rows non-CONDITIONAL) and
the fork verdict inputs (count of client-side positively-measured live
un-suppressed shared-world classes).

Suppressed-set JOIN is done against the LIVE source at analysis time (the
pre-registration requires it): kNpcAllowlist in npc_sync.cpp + the
ambient_spawner_suppress targets. Nothing is hand-copied.

Usage:
  python tools/rng_census_analyze.py            # default HOST + CLIENT_1 logs
  python tools/rng_census_analyze.py hostlog clientlog

Read-only analysis tool (RULE-2-exempt probe tooling).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The 16 pre-registered T1-1 rows (docs/COOP_RNG_AUTHORITY.md T1-1 table,
# pinned at pre-registration -- no analysis-time re-basing). Each row lists
# the substrings that identify its ticker/spawner class AND its product
# class(es) in the census records.
# (name, spawner-class substrings [driver/arm evidence], product substrings
#  [RESIDUE-only evidence -- a product's own Delay arms are its AI, not the roll])
T11_ROWS = [
    ("deerSpawner",        ["deerSpawner"],        ["deer_C"]),
    ("mannequinSpawner",   ["mannequinSpawner"],   ["wMannequinSpawn"]),
    ("hexahiveSpawner",    ["hexahiveSpawner"],    ["hexahive"]),
    ("eyers",              ["ticker_eyers"],       ["eyer_C"]),
    ("bp7Spawner",         ["bp7Spawner"],         ["bp7_C"]),
    ("roachSummoner",      ["roachSummoner"],      ["roach_C", "cockroach"]),
    ("grayBoar/boarInvasion", ["grayBoarSpawner", "boarInvasion"], ["grayboar"]),
    ("ghostcarSpawner",    ["ghostcarSpawner"],    ["ghostcar_C"]),
    ("bunnySpawner",       ["bunnySpawner"],       ["ominousBunny", "superAngryBunny"]),
    ("bloodSkeletonSpawner", ["bloodSkeletonSpawner"], ["bloodSkeleton"]),
    ("arirBusterSpawner",  ["arirBusterSpawner"],  ["fakeRadarWalker"]),
    ("greenFireSpawner",   ["greenFireSpawner"],   ["greenfire_C"]),
    ("furfurAltarSpawner", ["furfurAltarSpawner"], ["paranormalSpot"]),
    ("hillRollerSpawner",  ["hillRollerSpawner"],  ["propThrown"]),
    ("ufoDropper",         ["ufoDropper"],         ["fallingBody"]),
    ("yellowWispSpawner",  ["yellowWispSpawner"],  []),
]

# Statically-adjudicated CONDITIONAL rows (pre-work gate 3: NO drivers in own
# bytecode = externally event-triggered). A positive measurement overrides.
STATIC_CONDITIONAL = {"greenFireSpawner", "furfurAltarSpawner",
                      "hillRollerSpawner", "arirBusterSpawner"}

FIRST_SIGHT = re.compile(
    r"\[RNG-CENSUS\] first-sight class='([^']+)' native=(\S+) role=(\S+) "
    r"coopOrigin=(\d) episode=(\d)")
TICKERS = re.compile(
    r"\[RNG-CENSUS\]\[TICKERS\] class='([^']+)' instances=(\d+) "
    r"tickEnabled=(\d+) role=(\S+) episode=(\d)")
RESIDUE = re.compile(
    r"\[RNG-CENSUS\]\[RESIDUE\] class='([^']+)' newActors=(\d+) "
    r"UNEXPLAINED=(\d+) role=(\S+)")
DUMP_ROW = re.compile(
    r"\[RNG-CENSUS\]\[DUMP\]\s+'([^']+)' (\S+): n=(\d+) coop=(\d+) "
    r"episode=(\d+) host=(\d+)")
QUIT = re.compile(r"\[RNG-CENSUS\]\[QUITGAME\]")


def suppressed_sets():
    """JOIN against the live source: npc allowlist + ambient suppress targets."""
    out = set()
    npc = (ROOT / "src/votv-coop/include/ue_wrap/sdk_profile_names.h").read_text(
        encoding="utf-8", errors="replace")
    m = re.search(r"kNpcAllowlist\[\]\s*=\s*\{(.*?)\};", npc, re.S)
    if m:
        # entries are NpcClass_* name constants -- resolve each to its L"..." class string
        for const in re.findall(r"(NpcClass_\w+)", m.group(1)):
            cm = re.search(const + r'\s*(?:\[\])?\s*=\s*L"([^"]+)"', npc)
            if cm:
                out.add(cm.group(1))
    amb = None
    for cand in ("src/votv-coop/src/coop/world/spawn_authority.cpp",):
        p = ROOT / cand
        if p.exists():
            amb = p.read_text(encoding="utf-8", errors="replace")
            break
    ambient = set(re.findall(r'L"(\w+Spawner\w*|\w*mushroom\w*|\w*pinecone\w*)"',
                             amb)) if amb else set()
    return out, ambient


def parse(path):
    rec = {"first": [], "tick": [], "residue": [], "dump": [], "quit": 0}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[RNG-CENSUS]" not in line:
                continue
            if (m := FIRST_SIGHT.search(line)):
                rec["first"].append(m.groups())
            elif (m := TICKERS.search(line)):
                rec["tick"].append(m.groups())
            elif (m := RESIDUE.search(line)):
                rec["residue"].append(m.groups())
            elif (m := DUMP_ROW.search(line)):
                rec["dump"].append(m.groups())
            elif QUIT.search(line):
                rec["quit"] += 1
    return rec


def match_row(name_substrings, cls):
    return any(s.lower() in cls.lower() for s in name_substrings)


def adjudicate(client, allow, ambient):
    rows = []
    for row_name, sp_subs, prod_subs in T11_ROWS:
        verdict, evidence = "CONDITIONAL", []
        supp = {c for c in (allow | ambient) if match_row(sp_subs, c)}
        # LIVE evidence: SPAWNER-class driver arms / dump rows with coop=0 ep=0
        live_hits = [g for g in client["first"]
                     if match_row(sp_subs, g[0]) and g[2] == "CLIENT"
                     and g[3] == "0" and g[4] == "0"]
        dump_hits = [g for g in client["dump"]
                     if match_row(sp_subs, g[0]) and int(g[2]) > int(g[3]) + int(g[4])]
        # product classes count ONLY via RESIDUE (an unexplained spawned actor)
        residue_hits = [g for g in client["residue"]
                        if match_row(sp_subs + prod_subs, g[0]) and int(g[2]) > 0
                        and g[3] == "CLIENT"]
        armed_hits = [g for g in client["tick"]
                      if match_row(sp_subs, g[0]) and int(g[2]) > 0 and g[3] == "CLIENT"]
        starved_hits = [g for g in client["tick"]
                        if match_row(sp_subs, g[0]) and int(g[2]) == 0 and g[3] == "CLIENT"]
        # Suppress-join PRECEDENCE (pre-registered: cancelled classes read DONE,
        # not false-live): a fn-body cancel still lets the BeginPlay-time arm
        # record fire, so arm-only evidence must not override the join. A
        # RESIDUE hit (an actual unexplained spawned actor) DOES override --
        # that means the cancel is broken; flag it loudly.
        if supp:
            if residue_hits:
                verdict = "live"
                evidence.append("!! SUPPRESSED CLASS WITH RESIDUE (cancel broken?): "
                                + "; ".join(f"{g[0]} unexplained={g[2]}" for g in residue_hits[:2]))
            else:
                verdict = "DONE-suppressed"
                evidence.append("suppress-set: " + ",".join(sorted(supp)))
                if live_hits or dump_hits:
                    evidence.append("(arm records present but body-cancelled -- eyeball: "
                                    + "; ".join(f"{g[0]}" for g in (live_hits + dump_hits)[:2]) + ")")
        elif live_hits or dump_hits or residue_hits:
            verdict = "live"
            for g in live_hits[:2]:
                evidence.append(f"first-sight {g[0]} native={g[1]}")
            for g in dump_hits[:2]:
                evidence.append(f"dump {g[0]} {g[1]} n={g[2]} coop={g[3]} ep={g[4]}")
            for g in residue_hits[:2]:
                evidence.append(f"residue {g[0]} unexplained={g[2]}")
        elif armed_hits:
            verdict = "armed"
            for g in armed_hits[:2]:
                evidence.append(f"ticker {g[0]} inst={g[1]} tickEnabled={g[2]}")
        elif starved_hits:
            verdict = "confirmed-starved"
            for g in starved_hits[:2]:
                evidence.append(f"ticker {g[0]} inst={g[1]} tickEnabled=0")
        elif row_name in STATIC_CONDITIONAL:
            verdict = "CONDITIONAL"
            evidence.append("static: no drivers in own bytecode (gate 3)")
        rows.append((row_name, verdict, "; ".join(evidence) or "no records"))
    return rows


def main():
    host_log = (sys.argv[1] if len(sys.argv) > 1 else
                str(ROOT / "Game_0.9.0n_HOST/WindowsNoEditor/VotV/Binaries/Win64/multivoid.log"))
    client_log = (sys.argv[2] if len(sys.argv) > 2 else
                  str(ROOT / "Game_0.9.0n_CLIENT_1/WindowsNoEditor/VotV/Binaries/Win64/multivoid.log"))
    host, client = parse(host_log), parse(client_log)
    allow, ambient = suppressed_sets()
    rows = adjudicate(client, allow, ambient)

    print(f"host records:   first={len(host['first'])} tick={len(host['tick'])} "
          f"dump={len(host['dump'])} residue={len(host['residue'])} quit={host['quit']}")
    print(f"client records: first={len(client['first'])} tick={len(client['tick'])} "
          f"dump={len(client['dump'])} residue={len(client['residue'])} quit={client['quit']}")
    print(f"suppress join: npc-allowlist={len(allow)} ambient={len(ambient)}\n")

    width = max(len(r[0]) for r in rows) + 2
    n_live = n_cond = 0
    for name, verdict, ev in rows:
        if verdict == "live":
            n_live += 1
        if verdict == "CONDITIONAL":
            n_cond += 1
        print(f"{name:<{width}} {verdict:<18} {ev}")

    n = len(rows)
    non_cond = n - n_cond
    print(f"\ncallable-check: {non_cond}/{n} adjudicated non-CONDITIONAL "
          f"(threshold >= {2*n//3}) -> {'CALLABLE' if non_cond*3 >= 2*n else 'NOT CALLABLE (extend the run)'}")
    print(f"fork inputs: {n_live} client-side positively-measured LIVE rows "
          f"(>=3 -> STRUCTURAL; <=2 both-event-gated -> targeted; else DEFER)")
    # Orthogonal lanes regardless of fork:
    if client["quit"]:
        print(f"ORTHOGONAL: client-side QUITGAME liveness observed ({client['quit']} records) "
              f"-> freeze that mainGamemode roll lane")
    # extra live classes beyond the 16 rows (mainGamemode etc.) for context
    seen = set()
    for g in client["first"]:
        if g[2] == "CLIENT" and g[3] == "0" and g[4] == "0":
            seen.add(g[0])
    known = {c for _, sp, pr in T11_ROWS for c in seen if match_row(sp + pr, c)}
    extra = sorted(seen - known)
    if extra:
        print("\nother client-live roll machinery (context, outside the 16-row denominator):")
        for c in extra:
            print("  " + c)


if __name__ == "__main__":
    main()
