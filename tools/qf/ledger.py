#!/usr/bin/env python3
"""ledger.py -- the /qf per-question ledger (docs/QF_ARC.md WP-3).

    ledger.py [--dir DIR] pass <phase> [--topic T]          open a pass scope: question|design|impl|diff
    ledger.py [--dir DIR] append <round> <reply.json|->      validate the critic's reply, verify its anchors, record it
    ledger.py [--dir DIR] set <round> <id> <status> [--cite "<citation>"]
    ledger.py [--dir DIR] attest <round>                     the primary attests the convergence bars for this round
    ledger.py [--dir DIR] stop <reason> [--note "..."]       one of the stop list (tools/qf/critic_schema.json)
    ledger.py [--dir DIR] status                             the OPEN set and the pass's state

DIR is the session scratchpad (default: cwd).  Two files, one writer: `qf_ledger.json` is the STATE;
`qf_thread.md` is the human record the skill already keeps -- `append` and `stop` also write a rendered
table / a `STOP:` line into it.  The critic's verbatim reply is the JSON itself, stored whole.

Exit 1 from `append` = the reply is discarded (the skill re-spawns the critic); reasons are printed.
"""
import argparse
import json
import re
import shlex
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "tools" / "qf" / "critic_schema.json"
PHASES = ("question", "design", "impl", "diff")
ANCHOR_CMDS = {"grep", "rg", "git", "ls", "wc", "find", "cat", "sed", "python", "md5sum", "stat"}
_LOC = re.compile(r"(?P<path>[\w./\\-]+\.(?:cpp|h|hpp|inc|py|rs|js|md|txt|json|ini|ps1|log|toml|cmake))"
                  r":(?P<line>\d+)")


def load_schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


class Ledger:
    def __init__(self, directory: Path):
        self.dir = directory
        self.json_path = directory / "qf_ledger.json"
        self.md_path = directory / "qf_thread.md"
        self.meta = load_schema()
        self.state = json.loads(self.json_path.read_text(encoding="utf-8")) if self.json_path.exists() \
            else {"passes": []}

    # ---- persistence ------------------------------------------------------------------------
    def save(self):
        self.json_path.write_text(json.dumps(self.state, indent=2, ensure_ascii=False), encoding="utf-8")

    def md(self, text: str):
        with self.md_path.open("a", encoding="utf-8") as f:
            f.write(text if text.endswith("\n") else text + "\n")

    @property
    def current(self) -> dict | None:
        return self.state["passes"][-1] if self.state["passes"] else None

    def need_pass(self) -> dict:
        p = self.current
        if p is None:
            sys.exit("no pass open: run `ledger.py pass <phase>` first")
        return p

    def round(self, n: int) -> dict:
        p = self.need_pass()
        for r in p["rounds"]:
            if r["n"] == n:
                return r
        sys.exit(f"round {n} is not recorded in pass {p['n']}")

    # ---- commands ---------------------------------------------------------------------------
    def cmd_pass(self, phase: str, topic: str | None):
        if phase not in PHASES:
            sys.exit(f"phase must be one of {PHASES}")
        n = len(self.state["passes"]) + 1
        self.state["passes"].append({"n": n, "phase": phase, "topic": topic or "",
                                     "opened": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                                     "rounds": [], "stop": None})
        self.save()
        self.md(f"\n## PASS {n} ({phase}){' -- ' + topic if topic else ''}\n")
        print(f"pass {n} ({phase}) opened")

    def cmd_append(self, n: int, source: str) -> int:
        p = self.need_pass()
        if p["stop"]:
            sys.exit(f"pass {p['n']} is stopped ({p['stop']['reason']}); open a new pass")
        try:
            reply = json.load(sys.stdin if source == "-" else open(source, encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"DISCARD: reply is not JSON: {e}")
            return 1
        problems = self.validate(reply, p["phase"])
        anchors = self.verify_anchors(reply)
        for a in anchors:
            if a["verified"] is False:
                problems.append(f"{a['id']}: anchor failed verification: {a['detail']}")
        if problems:
            print("DISCARD the reply and re-spawn the critic:")
            for x in problems:
                print("  - " + x)
            return 1
        questions = {q["id"]: {"status": "open", "cite": ""} for q in reply.get("unresolved", [])}
        rec = {"n": n, "recorded": datetime.now(timezone.utc).isoformat(timespec="seconds"),
               "reply": reply, "anchors": anchors, "questions": questions, "attested": False}
        p["rounds"] = [r for r in p["rounds"] if r["n"] != n] + [rec]
        p["rounds"].sort(key=lambda r: r["n"])
        self.save()
        self.md(self.render_round(p, rec))
        ceiling = self.meta.get("safetyCeilingRounds", 50)
        print(f"round {n} recorded: {len(questions)} question(s), "
              f"{sum(1 for a in anchors if a['verified'])} anchor(s) verified, "
              f"converged={reply.get('converged')} floor={reply.get('readOnlyFloor')}")
        if len(p["rounds"]) >= ceiling:
            print(f"SAFETY CEILING: {len(p['rounds'])} rounds -- write `ledger.py stop capped` and present the OPEN set")
        return 0

    def validate(self, reply: dict, phase: str) -> list[str]:
        s = self.meta["schema"]
        out = []
        for k in s["required"]:
            if k not in reply:
                out.append(f"missing top-level field {k!r}")
        proof = reply.get("proofOfRead") or {}
        for k in ("qfDoc", "opusDoc"):
            if not isinstance(proof.get(k), str) or not proof.get(k).strip():
                out.append(f"proofOfRead.{k} missing (run verify_proof.py --reply on it before append)")
        qs = reply.get("unresolved")
        if not isinstance(qs, list):
            out.append("unresolved must be a list")
            qs = []
        converged, floor = bool(reply.get("converged")), bool(reply.get("readOnlyFloor"))
        if converged and floor:
            out.append("converged and readOnlyFloor cannot both be true")
        if not (converged or floor) and not (2 <= len(qs) <= 4):
            out.append(f"a non-terminal reply carries 2-4 questions, got {len(qs)}")
        if (converged or floor) and not str(reply.get("anchor", "")).strip():
            out.append("a converged / read-only-floor reply must carry its own anchor (the last lookup that found nothing)")
        if converged and any(not q.get("runtimeGated") for q in qs):
            out.append("converged with open non-runtime-gated questions")
        if floor and not qs:
            out.append("readOnlyFloor with no runtime-gated question named")
        if floor and any(not q.get("runtimeGated") for q in qs):
            out.append("readOnlyFloor while a question is not runtimeGated")
        if (converged or floor) and not isinstance(reply.get("convergenceRationale"), list):
            out.append("convergenceRationale must be a list of '<id>: closed by <citation>'")
        angles = set(s["properties"]["unresolved"]["items"]["properties"]["angle"]["enum"])
        req = s["properties"]["unresolved"]["items"]["required"]
        anchored = 0
        for i, q in enumerate(qs):
            qid = q.get("id", f"#{i + 1}")
            for k in req:
                if not str(q.get(k, "")).strip():
                    out.append(f"{qid}: missing field {k!r}")
            if q.get("angle") not in angles:
                out.append(f"{qid}: angle {q.get('angle')!r} is not in the closed list")
            if not str(q.get("q", "")).strip().endswith("?"):
                out.append(f"{qid}: q must be a question ending in '?'")
            if self.anchor_kind(q.get("anchor", "")) in ("loc", "cmd"):
                anchored += 1
            elif phase == "diff":
                out.append(f"{qid}: a DIFF pass question must anchor on a path:line or a command")
        if qs and anchored == 0 and not (converged or floor):
            out.append("no question anchored on a path:line or a command (one per round is mandatory)")
        return out

    @staticmethod
    def anchor_kind(anchor: str) -> str:
        a = (anchor or "").strip()
        if _LOC.search(a):
            return "loc"
        try:
            argv = shlex.split(a.split("=")[0].split("->")[0])
        except ValueError:
            argv = []
        if argv and argv[0] in ANCHOR_CMDS:
            return "cmd"
        return "quote" if a else "none"

    def verify_anchors(self, reply: dict) -> list[dict]:
        items = [(q.get("id", f"#{i + 1}"), q.get("anchor", "")) for i, q in enumerate(reply.get("unresolved", []))]
        if reply.get("converged") or reply.get("readOnlyFloor"):
            items.append(("verdict", reply.get("anchor", "")))
        out = []
        for qid, anchor in items:
            kind = self.anchor_kind(anchor)
            rec = {"id": qid, "anchor": anchor, "kind": kind, "verified": None, "detail": ""}
            if kind == "loc":
                m = _LOC.search(anchor)
                path = ROOT / m.group("path").replace("\\", "/")
                line = int(m.group("line"))
                if not path.exists():
                    rec.update(verified=False, detail=f"{m.group('path')} does not exist")
                else:
                    total = sum(1 for _ in path.open(encoding="utf-8", errors="replace"))
                    rec.update(verified=line <= total,
                               detail=f"line {line} of {total}" if line <= total else f"line {line} > {total} lines")
            elif kind == "cmd":
                cmd = anchor.split("=")[0].split("->")[0].strip()
                try:
                    r = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True, timeout=30)
                    first = (r.stdout.strip().splitlines() or [""])[0][:120]
                    claimed = re.search(r"=\s*(\d+)\s*$", anchor.strip())
                    if claimed and first.strip().isdigit():
                        same = int(first.strip()) == int(claimed.group(1))
                        rec.update(verified=same, detail=f"claimed {claimed.group(1)}, got {first.strip()}"
                                   + ("" if same else " -- the anchor's number does not reproduce"))
                    else:
                        rec.update(verified=True, detail=f"exit {r.returncode}; first line: {first!r}")
                except (OSError, subprocess.TimeoutExpired) as e:
                    rec.update(verified=False, detail=f"command failed: {e}")
            elif kind == "quote":
                rec.update(verified=None, detail="quote anchor (not re-run)")
            out.append(rec)
        return out

    def cmd_set(self, n: int, qid: str, status: str, cite: str):
        if status not in self.meta["statuses"]:
            sys.exit(f"status must be one of {self.meta['statuses']}")
        r = self.round(n)
        if qid not in r["questions"]:
            sys.exit(f"round {n} has no question {qid!r}; it has {list(r['questions'])}")
        if status == "answered-measured" and not (_LOC.search(cite or "") or "/" in (cite or "")):
            sys.exit("answered-measured needs the PRIMARY's own citation (a path:line or a log path) via --cite")
        if status in ("withdrawn-with-reason", "runtime-gated", "handed-to-user") and not (cite or "").strip():
            sys.exit(f"{status} needs its reason / probe / question via --cite")
        r["questions"][qid] = {"status": status, "cite": cite or ""}
        self.save()
        self.md(f"- ledger: round {n} {qid} -> **{status}** {cite}".rstrip())
        print(f"round {n} {qid} -> {status}")

    def cmd_attest(self, n: int):
        r = self.round(n)
        r["attested"] = True
        self.save()
        self.md(f"- ledger: round {n} convergence bars ATTESTED by the primary")
        print(f"round {n}: bars attested")

    def cmd_stop(self, reason: str, note: str):
        if reason not in self.meta["stops"]:
            sys.exit(f"reason must be one of {self.meta['stops']}")
        p = self.need_pass()
        if reason == "user-cap" and not note:
            sys.exit("user-cap needs --note N")
        p["stop"] = {"reason": reason, "note": note or "",
                     "at": datetime.now(timezone.utc).isoformat(timespec="seconds"), "rounds": len(p["rounds"])}
        self.save()
        self.md(f"\nSTOP: {reason}{' -- ' + note if note else ''}\n")
        print(f"STOP: {reason}" + (f" -- {note}" if note else ""))

    # ---- state ------------------------------------------------------------------------------
    def open_set(self, p: dict) -> list[tuple]:
        rows = []
        for r in p["rounds"]:
            for qid, q in r["questions"].items():
                if q["status"] in ("open", "answered-inferred", "runtime-gated", "handed-to-user"):
                    rows.append((r["n"], qid, q["status"]))
        return rows

    def cmd_status(self):
        p = self.current
        if p is None:
            print("no pass open")
            return
        rows = self.open_set(p)
        last = p["rounds"][-1] if p["rounds"] else None
        if p["stop"] and p["stop"]["reason"] == "capped":
            state = "CAPPED (abnormal): present the OPEN set as a residual"
        elif last and last["reply"].get("converged") and last["attested"] and not rows:
            state = "CONVERGED"
        elif last and last["reply"].get("readOnlyFloor") and last["attested"] and rows \
                and all(s == "runtime-gated" for _, _, s in rows):
            state = "READ-ONLY FLOOR: every open question is runtime-gated; the next probe is named"
        else:
            state = "OPEN"
        print(f"pass {p['n']} ({p['phase']}) rounds={len(p['rounds'])} stop={p['stop']['reason'] if p['stop'] else '-'}")
        print(f"state: {state}")
        if rows:
            print("open set:")
            for n, qid, s in rows:
                print(f"  round {n} {qid}: {s}")
        if last and (last["reply"].get("converged") or last["reply"].get("readOnlyFloor")) and not last["attested"]:
            print("the critic returned a verdict but the primary has not attested the bars: `ledger.py attest <round>`")

    def render_round(self, p: dict, r: dict) -> str:
        reply = r["reply"]
        lines = [f"\n### Round {r['n']} -- ledger (pass {p['n']}, {p['phase']})",
                 f"credit: {reply.get('credit', '')}",
                 "", "| id | angle | q | anchor | verified |", "|---|---|---|---|---|"]
        ver = {a["id"]: a for a in r["anchors"]}
        for q in reply.get("unresolved", []):
            a = ver.get(q.get("id"), {})
            lines.append(f"| {q.get('id')} | {q.get('angle')} | {q.get('q', '').replace('|', '/')} | "
                         f"{str(q.get('anchor', '')).replace('|', '/')} | {a.get('verified')} {a.get('detail', '')} |")
        if reply.get("converged") or reply.get("readOnlyFloor"):
            lines.append(f"verdict: {'converged' if reply.get('converged') else 'read-only floor'} -- anchor: "
                         f"{reply.get('anchor', '')}; rationale: {'; '.join(reply.get('convergenceRationale') or [])}")
        lines.append("")
        lines.append("### Critic (verbatim)")
        lines.append("```json")
        lines.append(json.dumps(reply, indent=2, ensure_ascii=False))
        lines.append("```")
        return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=".", help="the session scratchpad holding qf_thread.md")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pass"); s.add_argument("phase"); s.add_argument("--topic")
    s = sub.add_parser("append"); s.add_argument("round", type=int); s.add_argument("source")
    s = sub.add_parser("set"); s.add_argument("round", type=int); s.add_argument("id"); s.add_argument("status"); s.add_argument("--cite", default="")
    s = sub.add_parser("attest"); s.add_argument("round", type=int)
    s = sub.add_parser("stop"); s.add_argument("reason"); s.add_argument("--note", default="")
    sub.add_parser("status")
    a = ap.parse_args(argv)
    led = Ledger(Path(a.dir).resolve())
    if a.cmd == "pass":
        led.cmd_pass(a.phase, a.topic)
    elif a.cmd == "append":
        return led.cmd_append(a.round, a.source)
    elif a.cmd == "set":
        led.cmd_set(a.round, a.id, a.status, a.cite)
    elif a.cmd == "attest":
        led.cmd_attest(a.round)
    elif a.cmd == "stop":
        led.cmd_stop(a.reason, a.note)
    elif a.cmd == "status":
        led.cmd_status()
    return 0


if __name__ == "__main__":
    sys.exit(main())
