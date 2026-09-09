#!/usr/bin/env python3
"""prose_drill_fixtures -- the throwaway repository the drill measures.

One document and one source file carrying every class of marker the gate counts,
plus the smaller files that pin a specific answer: a script, an ignore file whose
rules are data rather than prose, a manifest, a clean source that must score
zero, and the headers that decide who owns a pinned offset.

Deliberately not named public_prose_*: that glob is the set the mutation pass
searches, and a fixture is full of the very text an anchor is made of.
"""

DOC = """# A doc

USER said, verbatim: «делай». A /qf round and two audit agents agreed on 2026-09-05.
See memory/feedback_x.md, research/runs/x, CLAUDE.md, .claude/skills and docs/security/TRACKER.md.
A wiki link [[lesson-some-slug]] is the same pointer in the notation memory uses.
A [tracked link](../README.md), an [external](https://example.com), and a [dead one](../nowhere.md).
A backticked `docs/nowhere.md` names no file; a backticked `docs/a.md` does.
An [uppercase scheme](HTTPS://example.com/x), an [encoded name](My%20File.md), a [backslash](sub\\c.md).

```
a [link in a code block](nothing.md) is not a link
```
"""
SRC = """#include "d.h"
#include <nlohmann/json.hpp>
// 2026-09-05: the USER asked for this, verbatim; a /qf round and an agent agreed.
// see research/findings/x.md and CLAUDE.md, lesson 12, commit deadbeef12
// a wiki link like [[feedback-some-rule]] points at memory the same as memory/x.md does
// PRECISION: a[[i]], [[ab]] and the C++ attribute [[no_unique_address]] are not slugs
// the files are NAMED with underscores: [[feedback_probe_dont_guess_rule]] is the same pointer
// docs/nowhere.md names no tracked doc; docs/a.md does
// the field sits at 0x04F8, a number pinned in prose that this file's code never reads
// PRECISION: opcode 0x45, sentinel 0xFF and colour 0x40 are hex, not offsets, and must NOT count
// OWNERSHIP: the translation at +0x10 is read three lines down, so the number IS the fact here
// RECALL: naming hiddenByAComment() here must NOT rescue it from the dead list
// the retry came from a review nobody outside can read
// two audits agreed; a third audited it; a fourth is auditing it now
// AUDIT F-3 in capitals is the same citation
// `[V]` an evidence tag from our working docs, which source has no legend for
// a bare [?] on its own line
// and a [RD] on its own
int calledOnce() { return 0; }
// one label family per line, so dropping any one of them turns this drill red
// CRIT-1 names a review finding
// security A34 names a row of a register that is not published
// Inc-2 names a build increment
// Inc3 names the same increment with the dash left out
// Increment 2b spells the word out, which is the same citation again
// increment-1 in lower case is that citation once more
// take-9 names an attempt
// WP-2 names a work package
// the s28 cut named a session
// finding 3 named itself
// K-5: a bare label counts when it opens the comment
// v133 names the build a change landed in, which a reader cannot look up
// PRECISION: IPv4, a path like tools/v12/x, a decimal 1.v20 and a bare v9 are not build tags
// a colon does too, as in R-2: the shared-scan consumer
// PRECISION: an auditorium, a bracketed [Value], an [ok] flag, leaving N-1 rows and
// a balance of X -> X-93 are none of them
// `[A]` is an evidence tag of the same family as the three above
int neverCalledAnywhere() { return calledOnce(); }
int hiddenByAComment() { return 0; }
int sameNameAsALiveOne() { return 0; }
int useTheLiveTwin() { return sameNameAsALiveOne(); }
int f() { return 0; } // trailing comments are code lines for the VOLUME counters
// POSITION: a citation is a citation wherever it sits, so the MARKER counters read the tail of a
// code line too. One marker per line, so dropping the tails cannot stay green on a survivor.
int t1() { return 0; } // WP-4 names a work package from a trailing comment
int t2() { return 0; } // this one was settled by an audit nobody outside can read
int t3() { return 0; } // added 2026-09-06, which is a diary entry wherever it sits
// a month with no day, as 2026-08: opens a diary entry too
// PRECISION: 0x2026-08 is hex and a dash, and v2026-08 names no day either
// PRECISION: a bare year 2026, a port 20260 and a range 2000-24 are none of them dates
int t4() { return 0; } /* a trailing BLOCK comment carries a citation too: take-9 */
// one user attribution per line, so dropping any one of them turns this drill red. The capitals
// form needs a line of its OWN: every earlier one read "USER asked" or "USER said", which the
// lower-case noun group matches too, so the alternative the counter began as was unobserved.
// flagged as USER in the ledger
// per the user, this row stays
// user: "the words themselves, quoted"
// user: the same attribution paraphrased, which opens no quote
// the user-requested shape of the panel
// user 2026-07-04 named the default
// the user asked for this one
// a user report settled which of the two it was
// a user req shrank the radius
// a user retest confirmed it
// the user said so at the time
// the user's choice of the two
// a user decision, recorded
// the user rule that governs it
int t5() { return 0; }
// PRECISION: the user types into a user widget as user 0, and the user unchecked "Custom colour"
// PRECISION: a per-user setting, a multi-user session and the user-visible form are all prose
// PRECISION: two files, both the mechanism rather than a user:
// the colon above ends its line, so it introduces this one instead of attributing anything
const char* d = /* closes here */ "docs/nowhere.md";  // the string is CODE, not comment
float translation(const char* base) { return *(const float*)(base + 0x10); }
const char* s = "// not a comment";
/* a block
   comment */
// line 0 of a long block
// line 1 of a long block
// line 2 of a long block
// line 3 of a long block
// line 4 of a long block
// line 5 of a long block
// line 6 of a long block
// line 7 of a long block
// line 8 of a long block
// line 9 of a long block
// line 10 of a long block
// line 11 of a long block
// line 12 of a long block
// line 13 of a long block
// line 14 of a long block
// line 15 of a long block
int g() { return 1; }
// a survey row cited with no slug at all, as `row :79` is
int r1() { return 0; } // and the same citation carrying its slug, as census row a_file:138 does
int r2() { return 0; } // plural too, as census rows a_file:192/:196
// PRECISION: a row that is a MIRROR -> a 1:1, rows 1 and 2, and a ratio of 3:4 are all prose
// a line of a file version that no longer exists, as x.cpp's old :436-783 block is
// PRECISION: a deliberate :7777 and the default :80/:443 are ports, not a line of anything
// a citation of a line a file no longer has, as x.cpp:9000 is in a file this short
// a citation of a file the tree does not carry at all, as gone.cpp:12 is
// PRECISION: x.cpp:1 resolves -- the file is here and it is that long, so it is not debt
// PRECISION: vendored.cpp:9000 resolves too -- a submodule is pinned and cannot be shortened
// one document spelling per line, so dropping any one of them turns this drill red
// COOP_EVENT_JOIN.md names a document with no directory in front of it and none in the tree
// DEATH_ARC section 3.1 is the same name with the extension dropped and a locator in its place
// the design doc settled it, which is a document with no name a reader can look up at all
// PRECISION: docs/TRACKED.md named bare, as TRACKED.md, resolves by BASENAME and is not debt
// PRECISION: MY_PORT (10001) is configuration, and the tail of votv-x-design-SOMEWHERE.md
// belongs to the pointer that names the finding, not to a document of its own
int p0() { return 0; }
// ---- the five citation axes: each one cites something the tree itself can answer ----
// PATH: a repository path that resolves to nothing, as src/coop/net/gone.cpp does
int p1() { return 0; }
// PRECISION: src/x.cpp resolves in the MODULE-ROOT spelling this tree writes, include/d.h does
// too, third_party/vend/vendored.cpp resolves inside a submodule without reading it, and
// Runtime/Core/Public/Math/Rotator.h names UE's tree, which this repository never carried
// PRECISION: include/v.h.in is a template whose name a pattern that stops at the `.h` reads
// as dead; nlohmann/json.hpp is an include spelling, which this file declares at its top and
// which no repository has to carry
// PRECISION: include/gone.h.bak ends in no extension this reads, so it names nothing here;
// stopping at the shorter name would invent a dead path out of a live file
int p2() { return 0; }
// LOG: a marker no format in the tree sends, as "zzz_sync: nothing logs this line" is
// PRECISION: "x_sync: applied active=1" IS sent -- the format writes %d where the comment writes
// the value a reader sees, and comparing the two the other way round calls every one of them dead
int p3() { return 0; }
// ENV: VOTVCOOP_NOT_READ_ANYWHERE names a variable no code reads
// PRECISION: VOTVCOOP_READ_HERE is read below, and VOTVCOOP_RUN_ is a family, not a name
int p4() { return 0; }
// INI: `net.no_such_row=1` names a row the registry does not carry
// PRECISION: `net.real_row=1` is one it does
int p5() { return 0; }
// MEMBER: Drilled::noSuchMember names a member this tree's own type does not have
// PRECISION: Drilled::realMember is one it has, and IDXGISwapChain::Present names a type this
// tree only forward-declares, whose real methods are DirectX's and not ours to adjudicate
// MEMBER: dspace::Drilled::alsoMissing is the namespace-qualified form this tree actually
// writes; a pattern that does not overlap consumes dspace as the type and never reads the member
// PRECISION: FMirror::NoSuchMethod names an ENGINE value type this tree mirrors as a POD under
// the engine's own name, so its real methods are not ours to adjudicate
// PATH: Game_0.9.0n/Content/Paks/x.ini sits in the game's own tree, several segments in
// INI: `net.no_such_row = 1` with spaces is the same row citation
// PRECISION: `net.no_such_row == 3` in backticks is a comparison, not an ini row
struct IDXGISwapChain;
namespace dspace { struct Drilled { int realMember; }; }
CFG_FLAG(net_real_row, "net.real_row", "net", false, "VOTVCOOP_READ_HERE", "a row")
void logs() { UE_LOGI("x_sync: applied active=%d", 1); }
// ---- the string axis: a citation is a citation wherever it sits, literals included ----
// PRECISION: a comment may quote "the v88 build tag" without the STRING axis reading it too
const char* sm1 = "the v133 build tag, in a line a player pastes into a bug report";
const char* sm2 = "security A65 names a row of a register that is not published";
const char* sm3 = "added 2026-09-06, which is a diary entry inside a string as much as outside";
const char* sm4 = "the user asked for this shape";
const char* sm5 = "settled in a /qf round";
const char* sm6 = "// v99 keeps its marker: the literal is lexed, not cut at the slashes";
const char* sm7[] = {"the v41 tag", "and the WP-3 label, both on one line"};
const char* sm8 = "\u043f\u0440\u0438\u0432\u0435\u0442 -- the fold selftests need this, and the axis must NOT read it";
// PRECISION: three families are exempt in a LITERAL because they are ordinary product English
const char* sm9 = "save: the audit of slot 3 failed, the agent muted, lesson learned";
// the compiler joins these two, so the marker astride the join has to be read
const char* sm10 = "bounded per security " "A65 -- refused";
int bc() { return 0; } /* closes here */ const char* sm11 = "the v42 tag after a block comment";
const char* sm12 = R"(a raw string whose "v43" sits inside its own quotes)";
// a comment between two literals does not end the concatenation, and the marker is astride it
const char* sm13 = "the v4"   // the join has to survive this
                   "4 tag, split across a comment";
"""

# The other.* class: a script a contributor runs, an ignore file, and a manifest. Whole lines
# are measured here, except in an ignore file where only the # comments are. One marker per line.
OTHER_PY = """# a script
# see NOWHERE.md, a document this tree does not carry
notes = open("GONE2.md")  # PRECISION: a .md a script OPENS is data; only the comment is prose
# the design doc says so, which names one that cannot even be looked up
# PRECISION: docs/a.md and a bare TRACKED.md both resolve and are not debt
# dated 2026-09-05
# the USER asked for it
# said verbatim
# a /qf round
# an agent ran it
# see research/runs/x
# see memory/feedback_y.md
# see CLAUDE.md
# see docs/security/TRACKER.md
# a wiki link [[lesson-some-other-slug]]
print("делай")
"""
# PRECISION for the comments-only carve-out: the RULES name research/ and .claude/ and must NOT
# count, because a rule cannot ignore a path without naming it. Only the comment counts.
OTHER_IGNORE = """# PRECISION: GONE.md is named here to say WHY it is ignored, which is this file's job
# the notes are not published, decided 2026-09-05
research/
.claude/
"""
# PRECISION for the wikilink slug shape: a TOML array-of-tables header is not a pointer.
OTHER_TOML = """# a manifest
[[bin]]
[[test]]
name = "x"
"""

# a source that carries comment but no counter at all: it must count as SWEPT
SRC_CLEAN = """// What this does, in one line of present-tense fact.
// A second line, well under the block cap.
int h() { return 2; }
"""
# a header declaring one function the tree calls and one nothing calls
HDR = """#pragma once
int calledOnce();
int neverCalledAnywhere();
int hiddenByAComment();
int sameNameAsALiveOne();
"""
# The precision canary for attribution: this header declares a name that ALSO exists, live, in
# d.h. Counting the bare name tree-wide called both alive and hid a real dead capability.
HDR_TWIN = """#pragma once
int sameNameAsALiveOne();
"""
# the offsets belong here, so a pinned number in THIS file is not counted
HDR_OWNER = """#pragma once
// the profile owns offsets: 0x1234 here is the point of the file
inline constexpr int kThing = 0x1234;
"""
