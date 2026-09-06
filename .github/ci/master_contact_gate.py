#!/usr/bin/env python3
"""master_contact_gate -- census, by OPERATION, of everything that can tell an
outside server where this player is.

WHY THIS EXISTS. An external source review of the public tree (2026-08-30) asked a
question the project could not answer from its own code: can I run a server without
sending my IP to the master? Answering it took a census -- and the FIRST census was
wrong, because it was taken over the name of a helper (`http::Post` / `http::Get`)
rather than over the operation. Two whole classes were missing: the signaling leg
opens a RAW socket, and the ICE config hands GNS a STUN list and our own TURN
credentials. A gate that greps for `http::` would have reproduced exactly that
blind spot, so this one is written around the four classes instead.

WHAT IT HOLDS:

  1. Every master HTTP call site is a known one. A NEW endpoint, or an old one
     appearing in a new file, fails -- because "is this lane asked for by the
     player?" is a judgement, and a judgement belongs in review, not in a diff
     nobody looked at twice.

  2. `RefreshLatestVersion()` has exactly ONE caller, and it is the browser
     surface. This is the fix of b349de42 held in place: the update check used to
     fire at boot and on every main-menu entrance, so the master learned the
     player's address before they had made any multiplayer decision at all.

  3. Raw socket connects live only in the signaling client, and the STUN/TURN
     config writes only in ice_config. Either spreading is a new class of contact
     and must be a deliberate, reviewed act.

It is a CENSUS gate, not a policy gate: it cannot tell whether a lane is asked for.
It fails on anything it has not been told about, and the fix for a legitimate
addition is to add it here, in the same commit, with a reason.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src" / "votv-coop" / "src"

# --- class 1: the master's HTTP surface --------------------------------------
# file (relative to SRC) -> the set of endpoints it may call. Every one of these
# is downstream of an action the player took; see b349de42's message for the
# lane-by-lane reasoning.
ALLOWED_HTTP = {
    "coop/net/lobby_announcer.cpp": {"/v1/host", "/v1/heartbeat", "/v1/visibility", "/v1/leave"},
    # `<computed>` is the /v1/lobbies fetch: the path is assembled with query
    # parameters, so it reaches http::Get as a variable rather than a literal.
    # Declared explicitly so an unreadable endpoint still has to be admitted.
    "coop/net/lobby_client.cpp": {"<computed>", "/v1/latest", "/v1/join"},
}

# --- class 2: the one lane that must stay player-triggered -------------------
REFRESH_CALLER = "ui/server_browser_surface.cpp"

# --- classes 3 and 4: the raw socket and the ICE credentials -----------------
ALLOWED_RAW_CONNECT = {"coop/net/signaling_client.cpp"}
ALLOWED_ICE_CONFIG = {"coop/net/ice_config.cpp"}

# Two forms, and the second one is why this gate nearly shipped with the same blind
# spot the first census had. `lobby_client.cpp:46` calls `http::Get(masterUrl, path, ...)`
# where `path` is a VARIABLE, so a literal-only pattern silently counted six endpoints
# where there are seven -- an endpoint assembled at runtime is exactly the one a
# name-shaped census misses. A computed path is recorded as `<computed>` and must be
# declared like any other, so it cannot pass by being unreadable.
HTTP_CALL = re.compile(r"http::(?:Post|Get)\s*\(\s*[^,]+,\s*\"([^\"]+)\"")
HTTP_CALL_ANY = re.compile(r"http::(?:Post|Get)\s*\(")
# `connect(` also matches ConnectByIPAddress and friends, so reject an identifier
# character before it. NO whitespace before the paren: the first version allowed
# `\s*` and promptly flagged five files whose only sin was English -- a comment
# reading "// connect (client log ...)" and a log string "for connect (opacity...".
# A C call has no space there, and prose almost always does.
RAW_CONNECT = re.compile(r"(?<![A-Za-z0-9_>])connect\(")


# TWO views of the same file, because the two question classes need opposite things.
# The endpoint census READS string literals -- they are the answer. The raw-connect
# and ICE checks must IGNORE them, or a log line mentioning the word reads as a call.
# Collapsing both onto one "code" view stripped the literals out from under the
# endpoint scan and silently dropped the count from seven to two; the gate stayed
# GREEN-ish while measuring almost nothing. Kept separate, and the printed count is
# what catches that class if it happens again.
def no_comments(text):
    """`//` comments removed, string literals intact."""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def code_only(text):
    """`//` comments AND string literals removed.

    Crude on purpose -- it does not need to parse C++, only to stop a sentence
    inside a log message from reading as a socket call.
    """
    return "\n".join(re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
                     for line in no_comments(text).splitlines())
ICE_WRITE = re.compile(r"k_ESteamNetworkingConfig_P2P_(?:STUN|TURN)_\w+")
REFRESH = re.compile(r"\bRefreshLatestVersion\s*\(\s*\)")


def sources():
    for p in sorted(SRC.rglob("*.cpp")):
        yield p.relative_to(SRC).as_posix(), p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails = []
    seen_http = {}
    refresh_callers = set()

    for rel, text in sources():
        strings = no_comments(text)   # endpoints LIVE in the literals
        code = code_only(text)        # calls must not be read out of prose
        literal = HTTP_CALL.findall(strings)
        for ep in literal:
            seen_http.setdefault(rel, set()).add(ep)
        computed = len(HTTP_CALL_ANY.findall(strings)) - len(literal)
        if computed > 0:
            seen_http.setdefault(rel, set()).add("<computed>")
        if REFRESH.search(code):
            refresh_callers.add(rel)
        if RAW_CONNECT.search(code) and rel not in ALLOWED_RAW_CONNECT:
            fails.append(f"{rel}: opens a raw socket connection. Only "
                         f"{sorted(ALLOWED_RAW_CONNECT)} may -- a new one is a new class "
                         f"of contact that no http:: census would see.")
        if ICE_WRITE.search(code) and rel not in ALLOWED_ICE_CONFIG:
            fails.append(f"{rel}: writes STUN/TURN config. Only {sorted(ALLOWED_ICE_CONFIG)} "
                         f"may -- these hand a third party (and our relay) the player's address.")

    for rel, eps in sorted(seen_http.items()):
        allowed = ALLOWED_HTTP.get(rel)
        if allowed is None:
            fails.append(f"{rel}: calls the master ({', '.join(sorted(eps))}) and is not a "
                         f"known caller. Add it here with the reason it is player-triggered.")
            continue
        for ep in sorted(eps - allowed):
            fails.append(f"{rel}: new master endpoint '{ep}'. Say here why the player asked "
                         f"for it.")
    for rel in sorted(set(ALLOWED_HTTP) - set(seen_http)):
        fails.append(f"{rel}: listed as a master caller but calls nothing -- stale entry.")

    # The declaration and the definition both match the regex; neither is a call.
    callers = {r for r in refresh_callers
               if r not in ("coop/session/session_manager.cpp",)}
    if callers != {REFRESH_CALLER}:
        fails.append(f"RefreshLatestVersion callers are {sorted(callers) or '[]'}; the update "
                     f"check must be triggered from {REFRESH_CALLER} and nowhere else "
                     f"(b349de42: it used to fire at boot and on every menu entrance, so the "
                     f"master learned the player's address before they asked it anything).")

    for f in fails:
        print(f"master_contact_gate: FAIL: {f}")
    print(f"master_contact_gate: {len(fails)} FAIL "
          f"({sum(len(v) for v in seen_http.values())} master endpoints across "
          f"{len(seen_http)} files)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
