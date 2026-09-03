#!/usr/bin/env python3
"""census_git -- run git and read its output without losing a path to quoting.

One concept: the subprocess call the census layer is built on. It lives in its own module because
BOTH `status_census.py` and `census_history.py` need it and neither may import the other -- the
history store is the lower layer, and a shared primitive pushed into either one would make the
vocabulary of that module two things instead of one (the folder-per-domain-concept rule, applied at
file scale). Extracted 2026-09-03 with `status_census.py` at 1,244 LOC.
"""
import subprocess


def git(args, cwd, check=True, env=None, input_text=None):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env, input=input_text)
    if check and r.returncode != 0:
        raise RuntimeError("git {} failed ({}): {}".format(" ".join(args), r.returncode,
                                                            (r.stderr or r.stdout).strip()[:400]))
    return r.stdout


def gitz(args, cwd):
    """A NUL-separated path list: git QUOTES non-ASCII paths in line mode (core.quotePath), -z never does."""
    return [p for p in git(list(args), cwd).split("\0") if p]

