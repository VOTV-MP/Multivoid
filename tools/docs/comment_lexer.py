#!/usr/bin/env python3
"""comment_lexer -- is a change to a source file COMMENT-ONLY?

Decided on the WHOLE FILE, not per diff line, by a string-aware lexer per language: the old blob and
the new blob each have their comment tokens removed -- Python by `tokenize`, C-family and `.inc` by
a state machine over quotes / `//` / `/* */`, PowerShell `#` and `<# #>` outside quotes, YAML `#`
outside quotes -- whitespace collapsed, and the two residues must be EQUAL. A file type with no
comment grammar is code by construction. (docs/DOCUMENTIZE_ARC.md WP-1(c), /qf round 6 Q4: a
per-line regex was blind to a `#` or `//` inside a string literal.) A close commit may carry such a
change: a stale claim in a code comment is a documentation claim wherever it lives.
"""
import io
import tokenize


# ----------------------------------------------------------------------------- comment-only lexer
CFAM = (".h", ".hpp", ".cpp", ".c", ".inc", ".rs")


def _strip_c(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            q = c
            out.append(c)
            i += 1
            while i < n and src[i] != q:
                if src[i] == chr(92) and i + 1 < n:
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                i += 1
            if i < n:
                out.append(q)
                i += 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _strip_hash(src, block=None):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if block and src.startswith(block[0], i):
            j = src.find(block[1], i + len(block[0]))
            i = n if j < 0 else j + len(block[1])
            continue
        if c in "\"'":
            q = c
            out.append(c)
            i += 1
            while i < n and src[i] != q:
                out.append(src[i])
                i += 1
            if i < n:
                out.append(q)
                i += 1
        elif c == "#":
            j = src.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _strip_py(src):
    try:
        toks = tokenize.generate_tokens(io.StringIO(src).readline)
        keep = (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT, tokenize.ENDMARKER)
        return " ".join(t.string for t in toks if t.type not in keep)
    except Exception:
        return None


def code_residue(src, ext):
    """The file with its comments removed, whitespace collapsed; None = no grammar (all code)."""
    if src is None:
        return None
    if ext in CFAM:
        r = _strip_c(src)
    elif ext == ".py":
        r = _strip_py(src)
    elif ext == ".ps1":
        r = _strip_hash(src, ("<#", "#>"))
    elif ext in (".yml", ".yaml", ".sh", ".toml"):
        r = _strip_hash(src)
    else:
        return None
    return None if r is None else " ".join(r.split())


def comment_only(old, new, ext):
    a, b = code_residue(old, ext), code_residue(new, ext)
    return a is not None and b is not None and a == b
