#!/usr/bin/env python3
"""schema_sync.py -- keep the workflow's CRITIC_SCHEMA equal to tools/qf/critic_schema.json (QF_ARC WP-3).

    schema_sync.py --check     exit 1 if tools/workflows/qf_root_loop.js drifted from the JSON source
    schema_sync.py --write     regenerate the JS block from the JSON source

The Workflow runtime cannot read files, so the JS carries a generated copy of the `schema` object between
two marker comments; the JSON file is the ONLY place to edit.  tools/workflows/ is gitignored (local-only),
so --check reports "absent" and exits 0 on a checkout without it.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tools" / "qf" / "critic_schema.json"
JS = ROOT / "tools" / "workflows" / "qf_root_loop.js"
BEGIN = "// BEGIN CRITIC_SCHEMA -- generated from tools/qf/critic_schema.json; edit THERE, then `python tools/qf/schema_sync.py --write`"
END = "// END CRITIC_SCHEMA"


def render() -> str:
    schema = json.loads(SRC.read_text(encoding="utf-8"))["schema"]
    return f"{BEGIN}\nconst CRITIC_SCHEMA = {json.dumps(schema, indent=2, ensure_ascii=False)}\n{END}"


def current_block(text: str):
    m = re.search(re.escape(BEGIN) + r".*?" + re.escape(END), text, re.S)
    if m:
        return m.group(0), m.span()
    m = re.search(r"^const CRITIC_SCHEMA = \{.*?^\}\n", text, re.S | re.M)   # the hand-written original
    return (m.group(0).rstrip("\n"), m.span()) if m else (None, None)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    a = ap.parse_args(argv)
    if not JS.exists():
        print(f"{JS.relative_to(ROOT)} absent (gitignored, local-only); nothing to sync")
        return 0
    text = JS.read_text(encoding="utf-8")
    block, span = current_block(text)
    want = render()
    if a.check:
        if block == want:
            print("CRITIC_SCHEMA in sync")
            return 0
        print("DRIFT: tools/workflows/qf_root_loop.js CRITIC_SCHEMA != tools/qf/critic_schema.json (run --write)")
        return 1
    if block is None:
        print("no CRITIC_SCHEMA block found in the workflow; cannot place the generated copy")
        return 1
    new = text[:span[0]] + want + ("\n" if not text[span[1]:].startswith("\n") else "") + text[span[1]:]
    JS.write_text(new, encoding="utf-8")
    print("CRITIC_SCHEMA written from the JSON source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
