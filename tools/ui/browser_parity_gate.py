#!/usr/bin/env python3
"""Does the DEFAULT server browser still do everything the FALLBACK one does?

WHY THIS EXISTS. On 2026-08-30 the native browser became the permanent default
(`b90b261d`, user: "делаем нативный браузер дефолтом вечным"). The ImGui browser stayed
as the fallback -- also the user's call -- so the tree now compiles TWO implementations of
one concept, which RULE 2 tolerates here only because keeping the fallback is a product
decision RULE 1 does not override. What nothing checked was whether the two agree.

They did not. The user found out by trying to play:

    "Щас тестил сервер браузер - нету возможности нигде по айпи подключиться - НИГДЕ."

Direct-IP connect existed, worked, and persisted its address -- on the surface that had
just stopped being the default. A census then found FOUR more capabilities in the same
state (the nickname field, the host-name box, the Locked checkbox, the lock column). The
user had reported one of five, because one is all a player trips over at a time.

So the root was never a missing field. It was a default that moved with no parity check,
and this file is that check. A capability that exists on one surface and not the other is
a FAILURE unless somebody wrote down why -- which turns silent drift into a decision.

HOW IT DECIDES, and why not by name. Each capability is proven by the OPERATION that
implements it -- the call that connects, the row that persists, the property that renders
-- never by a helper's name. This project has now twice answered a question wrongly by
grepping for a name it guessed: the direct-connect census for THIS defect searched
`ConnectByAddress|JoinDirect|ConnectToAddress|JoinByIp`, found nothing, and nearly
reported that direct connect did not exist anywhere in the mod. It is called
`ConnectDirect`. See `[[lesson-census-by-the-operation-not-by-the-name]]`.

IT PRINTS ITS CENSUS, not just its verdict. A scanner that stops matching passes every
input forever and silently; the only cheap witness that it still reaches what it claims to
cover is a count you can check against a hand census once.
See `[[lesson-a-gates-printed-count-catches-what-its-verdict-cannot]]`.

Exit 0 = parity holds (or every gap is declared). Exit 1 = an undeclared gap.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src" / "votv-coop" / "src" / "ui"

# The fallback surface: one file, the ImGui browser.
IMGUI = ["server_browser.cpp"]

# The default surface: the native screen is SPLIT across a screen, a row model and an
# action bar, so a capability may live in any of them. Listing all three is the point --
# a gate that looked only at `server_browser_native.cpp` would report the address box
# missing today, when it is in the action bar.
NATIVE = [
    "server_browser_native.cpp",
    "server_browser_rows.cpp",
    "server_browser_actions.cpp",
    "native_text_field.cpp",
]


@dataclass
class Capability:
    """One thing a player can DO in a server browser."""

    name: str
    # What the user would say they cannot do, in their words where we have them. This is
    # printed on failure, because "nickname missing" is a code fact and "a player cannot
    # change their name here" is the defect.
    symptom: str
    # The OPERATION that proves the capability is wired, as a regex. Match the call, the
    # registry row, or the rendered property -- never a helper's name.
    operation: str
    # A declared, deliberate divergence. Set it and the gate stops failing on this row,
    # but the reason is now in the tree where a reader will find it.
    divergence: str = ""
    hits: dict = field(default_factory=dict)


CAPABILITIES = [
    Capability(
        name="direct-ip connect",
        symptom="cannot connect by IP -- the user's 2026-08-30 report, verbatim: "
                "'нету возможности нигде по айпи подключиться - НИГДЕ'",
        operation=r"ConnectDirect\s*\(",
        divergence="DEFERRED BY THE USER 2026-08-30. It shipped on the default surface for "
                   "one build and they cut it on sight: 'нахуй оно там нужно вообще - это "
                   "дизайн говно у сервер браузера - не нужен прям в нем ввод'. The "
                   "capability is NOT cancelled -- `ConnectDirect` works and the fallback "
                   "still reaches it -- but WHERE the default surface offers it is part of "
                   "the browser redesign they asked for next session ('нужен дизайн сервер "
                   "браузера как у людей без костылей'). Delete this divergence when that "
                   "design lands; do not re-add a box to the browser to make the gate green.",
    ),
    Capability(
        name="direct address persisted",
        symptom="the address is retyped on every launch instead of being remembered",
        operation=r"rows::browser_lastdirect",
        divergence="RIDES the direct-ip row above: with no address box on the default "
                   "surface there is nothing to remember. The ini row itself is untouched "
                   "and the fallback still writes it, so a player who used one surface will "
                   "find their address waiting when the redesign gives it a home.",
    ),
    Capability(
        name="nickname entry",
        symptom="cannot change the name other players see, from the screen that shows it",
        operation=r"SetNickname\s*\(|rows::net_nick",
        divergence="DEFERRED BY THE USER 2026-08-30, with the address box and for the same "
                   "reason: no text entry belongs on the browser as it is designed today. "
                   "Still a real gap against the fallback -- a player cannot set the name "
                   "everyone else sees -- and still owed by the redesign.",
    ),
    Capability(
        name="host game",
        symptom="cannot start hosting from the browser",
        operation=r"host_window_native::Open\s*\(|host_save_picker::Open\s*\(",
    ),
    Capability(
        name="refresh the list",
        symptom="the list can only update on its own timer",
        # The operation is "ask the session manager to re-fetch", spelled `sm::Refresh` on
        # the fallback and reached through the action bar's outcome on the default. This
        # regex named neither on its first run and reported the fallback as LACKING a
        # refresh it has had all along (`server_browser.cpp:78,154`) -- the gate written to
        # enforce census-by-operation failed by census-by-guessed-name, on its own first
        # execution. Caught by checking the count, not by the verdict.
        operation=r"(sm|session_manager)::Refresh\s*\(|\"refresh\"",
    ),
    Capability(
        name="join a listed lobby",
        symptom="cannot join a server from the list",
        operation=r"JoinLobby\s*\(",
    ),
    Capability(
        name="locked-lobby marker",
        symptom="a password-locked server looks identical to an open one, so a player "
                "picks it, waits, and is refused",
        operation=r"\.locked\b|\blocked\s*\?",
    ),
    Capability(
        name="host name entry",
        symptom="cannot name the server other players will see in the list",
        # The native side derives it from the nickname today; that is a real divergence
        # and it is DECLARED rather than silently passing.
        operation=r"g_hostName|hostName",
        divergence="DECLARED 2026-08-30: the native Host window derives the server name "
                   "from the nickname ('<nick>'s game') because it had no text entry at "
                   "all. `ui/native_text_field` now exists, so this is the next increment "
                   "rather than a permanent difference -- when it lands, delete this "
                   "divergence and let the gate hold the parity.",
    ),
    Capability(
        name="locked-lobby toggle when hosting",
        symptom="cannot host a locked game from this surface",
        operation=r"g_hostLocked|\blocked\b.*Checkbox|Checkbox.*\blocked\b",
        divergence="DECLARED 2026-08-30: the lobby PASSWORD does not exist in the tree at "
                   "all (security A2 is open -- what shipped is the exchange it will ride, "
                   "not the secret), so the ImGui checkbox sets a flag with no enforcement "
                   "behind it. Porting a control that does nothing would be worse than the "
                   "gap. Revisit WITH A2, not before.",
    ),
]


def read(files: list[str]) -> dict[str, str]:
    out = {}
    for f in files:
        p = SRC / f
        if not p.exists():
            print(f"  !! {f} does not exist -- the surface moved and this gate is stale")
            continue
        # Strip line comments: a capability named only in prose ("Connect/Join will land
        # at the right when T7 adds it") is a PLAN, and counting it as an implementation
        # is exactly how a gate reports parity that is not there.
        text = p.read_text(encoding="utf-8", errors="ignore")
        text = re.sub(r"//[^\n]*", "", text)
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        out[f] = text
    return out


def main() -> int:
    imgui = read(IMGUI)
    native = read(NATIVE)
    if not imgui or not native:
        print("browser_parity_gate: FAIL -- a surface's files are missing entirely")
        return 1

    failures, declared = [], []
    print(f"browser_parity_gate: {len(CAPABILITIES)} capabilities, "
          f"{len(imgui)} fallback file(s) vs {len(native)} default file(s)")
    print()

    for cap in CAPABILITIES:
        rx = re.compile(cap.operation)
        in_imgui = [f for f, t in imgui.items() if rx.search(t)]
        in_native = [f for f, t in native.items() if rx.search(t)]
        mark = "  ok  "
        if in_imgui and not in_native:
            mark = "DECLARED" if cap.divergence else " GAP  "
            (declared if cap.divergence else failures).append(cap)
        elif in_native and not in_imgui:
            # The default having MORE is not a defect -- it is the direction the tree is
            # meant to move. Reported so the fallback's own drift stays visible.
            mark = " ahead"
        elif not in_imgui and not in_native:
            mark = " none "
            failures.append(cap)
        print(f"[{mark}] {cap.name:<32} fallback={len(in_imgui)} default={len(in_native)}"
              + (f"  ({', '.join(in_native)})" if in_native else ""))

    print()
    for cap in declared:
        print(f"DECLARED DIVERGENCE -- {cap.name}\n    {cap.divergence}\n")

    if failures:
        print(f"browser_parity_gate: FAIL -- {len(failures)} undeclared gap(s)\n")
        for cap in failures:
            print(f"  {cap.name}")
            print(f"    a player: {cap.symptom}")
            print(f"    proven by: /{cap.operation}/")
            print("    Fix it on the default surface, or add a `divergence=` saying why "
                  "not. Both are answers; silence is not.\n")
        return 1

    print("browser_parity_gate: PASS -- every capability the fallback has, the default "
          f"has too ({len(declared)} declared divergence(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
