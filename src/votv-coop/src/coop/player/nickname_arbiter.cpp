// coop/player/nickname_arbiter.cpp -- see coop/player/nickname_arbiter.h.

#include "coop/player/nickname_arbiter.h"

#include "coop/player/roster_ledger.h"
#include "coop/session/player_handshake.h"
#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <string>
#include <vector>

namespace coop::nickname_arbiter {
namespace {

// Every distinct display name we could produce for one stem, in the order the
// dense-smallest-free policy tries them. n == 1 is the bare stem.
//
// THE CAP DISPLACES THE STEM, and that is why uniqueness is checked on the
// RESULT rather than on the stem: "AAAAAAAAAAAAAAAAAAA" + "2" is 20 characters
// and can equal a DIFFERENT player's 20-character name that happens to end in
// '2'. Checking the stem would miss that collision entirely.
//
// The stem is the WHOLE requested name, trailing digits included -- so a kept
// "Pelmentor2" that meets another "Pelmentor2" becomes "Pelmentor22", not
// "Pelmentor3". Stripping digits to find a "real" stem would require deciding
// whether a trailing number is a suffix we once added or part of the name its
// owner chose, and nothing in the string says which ("Kerfur2" is a name).
//
// THE CAP IS IN CODEPOINTS, and `substr` here was the last unit-truncation in
// the mod. It could split a surrogate pair -- emoji have been accepted since the
// D1 denylist shipped -- and while ToUtf8 then drops the lone surrogate so the
// ini never holds ill-formed bytes, the arbiter would judge uniqueness on the
// string WITH it while every egress emitted the string WITHOUT it. Two names
// differing only in a halved emoji were "distinct" and drew identically.
std::wstring Candidate(const std::wstring& stem, int n) {
    if (n <= 1) return stem;
    const std::wstring suffix = std::to_wstring(n);   // ASCII digits: units == codepoints
    if (coop::text::CountCodepoints(stem) + suffix.size() <=
        coop::player_handshake::kNickMaxChars)
        return stem + suffix;
    const size_t keep = (coop::player_handshake::kNickMaxChars > suffix.size())
                            ? coop::player_handshake::kNickMaxChars - suffix.size()
                            : 0;
    return coop::text::CapCodepoints(stem, keep) + suffix;
}

// Simple case folding over exactly the cased scripts the repertoire draws --
// ASCII, Latin-1 and Cyrillic. Deliberately a table and not LCMapStringW: this
// is an AUTHORITY function, and an authority whose answer comes from the OS is
// an authority that can differ between two machines running the same build.
uint32_t FoldCase(uint32_t c) {
    if (c >= L'A' && c <= L'Z')                     return c + 32;
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7)  return c + 32;   // Latin-1 caps
    if (c >= 0x0400 && c <= 0x040F)                 return c + 80;   // Ё, Ђ, ...
    if (c >= 0x0410 && c <= 0x042F)                 return c + 32;   // А-Я
    if (c == 0x04C0)                                return 0x04CF;   // palochka
    // The supplement/extended blocks are laid out as adjacent capital/small
    // pairs; only the parity of where a run starts differs.
    if ((c >= 0x0460 && c <= 0x0481) || (c >= 0x048A && c <= 0x04BF) ||
        (c >= 0x04D0 && c <= 0x052F) || (c >= 0xA640 && c <= 0xA66D) ||
        (c >= 0xA680 && c <= 0xA69B))
        return c | 1u;                                                // even -> odd
    if (c >= 0x04C1 && c <= 0x04CD)                 return c + (c & 1u);  // odd -> even
    return c;
}

}  // namespace

std::wstring FoldKey(const std::wstring& name) {
    // ARC D2. Two changes, one root: the key must describe what a HUMAN SEES.
    //
    // (1) It folds in CODEPOINTS. Folding wchar_t units gave an astral character
    //     TWO keys where a BMP character has one, so 𠀀 and 中 -- both drawn as
    //     the same fallback box -- would not have collided.
    // (2) Every codepoint this build cannot DRAW folds to ONE sentinel, so names
    //     that render identically collide and one of them takes the numeric
    //     suffix arc B already ships. That is what makes "everyone has a unique
    //     nameplate" true on screen rather than merely true in the string
    //     domain, and it makes it true FONT-INDEPENDENTLY -- no donor budget can
    //     cover every script, so a coverage-based guarantee is always partial.
    //
    // ONE SENTINEL PER CODEPOINT, never a collapsed run: N absent characters
    // draw as N identical boxes, so a 2-character and a 3-character CJK name
    // ARE visually distinguishable and must keep distinct keys. The key mirrors
    // the pixels exactly, which is the whole point.
    //
    // THE SENTINEL IS U+FFFD, the character an absent codepoint actually DRAWS
    // as: ImFont::BuildLookupTable (imgui_draw.cpp:3700) picks the fallback
    // glyph from { U+FFFD, '?', ' ' }, and the repertoire bakes U+FFFD so the
    // first choice always wins. Any other sentinel would re-open the same hole
    // one level down -- a name containing a LITERAL U+FFFD renders exactly like
    // an out-of-repertoire one, so the two must fold to the same key. Here the
    // key is not a stand-in for the pixels; it IS the pixels.
    constexpr wchar_t kAbsent = 0xFFFD;
    std::wstring key;
    key.reserve(name.size());
    for (size_t i = 0; i < name.size(); ) {
        uint32_t cp = 0;
        const size_t units = coop::text::DecodeCodepoint(name, i, &cp);
        const wchar_t* at = name.data() + i;
        i += units;
        if (!coop::text::InRepertoire(cp)) { key.push_back(kAbsent); continue; }
        const uint32_t folded = FoldCase(cp);
        if (folded == cp) key.append(at, units);
        else              key.push_back(static_cast<wchar_t>(folded));  // cased => BMP
    }
    return key;
}

std::wstring AssignAgainst(const std::wstring& requested,
                           const std::vector<std::wstring>& taken) {
    // Uniqueness is enforced over the set of names ALREADY ASSIGNED -- never
    // over a key recomputed from this function's own output, which would not be
    // well-founded once a suffix displaces stem characters at the cap.
    for (int n = 1; n <= 64; ++n) {
        const std::wstring candidate = Candidate(requested, n);
        const std::wstring key = FoldKey(candidate);
        bool clash = false;
        for (const std::wstring& t : taken)
            if (FoldKey(t) == key) { clash = true; break; }
        if (!clash) return candidate;
    }
    UE_LOGE("nickname_arbiter: no free variant of '%ls' -- keeping the request",
            requested.c_str());
    return requested;
}

std::wstring Assign(int slot, const std::wstring& requested) {
    UE_ASSERT_GAME_THREAD("g_rows (nickname_arbiter::Assign)");
    // The ledger's occupied rows ARE the collision set (R4 of the 42-round pass:
    // key on the nick STORE, not on mirror-element existence -- they are not
    // co-timed). Excluding our own row is what makes this idempotent: a retried
    // Join re-arbitrates the same request against the same others and lands on
    // the same answer instead of walking the suffix upward.
    //
    // Ghost-freeness is the ledger's guarantee, not a second one bolted on here:
    // ReconcileFromSession runs death FIRST and unconditionally
    // (roster_ledger.cpp:289), so a reconnecting peer cannot collide with its own
    // un-reaped row. That matters more than it looks -- since the user's
    // 2026-07-28 decision the assigned name is PERSISTED, so a rename earned
    // against a ghost would follow the human into every future session.
    std::vector<std::wstring> taken;
    taken.reserve(coop::roster_ledger::kMaxSlots);
    for (int s = 0; s < coop::roster_ledger::kMaxSlots; ++s) {
        if (s == slot) continue;
        const coop::roster_ledger::Row& r = coop::roster_ledger::Get(s);
        if (r.occupied() && !r.nick.empty()) taken.push_back(r.nick);
    }
    const std::wstring assigned = AssignAgainst(requested, taken);
    // The host's record of the decision. Logged on EVERY arbitration, not only
    // on a rename: "asked X, got X, against N names" is what distinguishes
    // "nothing collided" from "the request never arrived" -- a distinction the
    // first drill could not make, because an empty request sanitizes to the
    // placeholder and then looks like a perfectly ordinary name.
    UE_LOGI("nickname_arbiter: slot %d asked '%ls' -> assigned '%ls' (vs %zu taken)",
            slot, requested.c_str(), assigned.c_str(), taken.size());
    return assigned;
}

// --- selftest ----------------------------------------------------------------
//
// Runs against the PURE core, so it touches no ledger row, needs no game thread
// and can run at any point in boot. It covers what no LAN drill can stage on
// demand: the cap-displacing suffix, the 20-character collision a stem check
// would miss, case folding, and the kept-name cases the user's 2026-07-28
// decision introduced.
bool RunNicknameArbiterSelftest() {
    int pass = 0, total = 0;
    auto check = [&](const std::wstring& got, const wchar_t* want, const char* what) {
        ++total;
        if (got == want) { ++pass; return; }
        UE_LOGE("nickname-arbiter selftest: FAIL %s -- got '%ls' want '%ls'", what,
                got.c_str(), want);
    };

    // The ask, verbatim: three "Pelmentor" become Pelmentor / 2 / 3.
    check(AssignAgainst(L"Pelmentor", {}), L"Pelmentor", "first Pelmentor");
    check(AssignAgainst(L"Pelmentor", {L"Pelmentor"}), L"Pelmentor2", "second Pelmentor");
    check(AssignAgainst(L"Pelmentor", {L"Pelmentor", L"Pelmentor2"}), L"Pelmentor3",
          "third Pelmentor");

    // Idempotent: re-arbitrating a name whose holder is EXCLUDED from the set
    // returns the same answer (a retried Join must not ratchet).
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor"}), L"Pelmentor2", "retry is stable");

    // USER DECISION 2026-07-28 -- the assigned name is KEPT, so a returning
    // Pelmentor2 asks for Pelmentor2 and keeps it when nobody else has it...
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor", L"Pelmentor3"}), L"Pelmentor2",
          "a kept name survives when free");
    // ...and the SECOND Pelmentor2 is the one that moves. The whole requested
    // name is the stem, so it becomes Pelmentor22 rather than Pelmentor3.
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor2"}), L"Pelmentor22",
          "a second kept name suffixes the whole stem");
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor2", L"Pelmentor22"}), L"Pelmentor23",
          "and keeps walking");

    // Case folding: PELMENTOR collides with Pelmentor.
    check(AssignAgainst(L"PELMENTOR", {L"pelmentor"}), L"PELMENTOR2", "fold is case-insensitive");

    // Dense smallest-free: the freed number is reused by the next joiner.
    check(AssignAgainst(L"Pelmentor", {L"Pelmentor", L"Pelmentor3"}), L"Pelmentor2",
          "dense reuse of a freed number");

    // THE CAP TRAP a stem check would miss: the "+2" variant of a 19-character
    // name is exactly the 20-character name ANOTHER player already holds.
    check(AssignAgainst(L"AAAAAAAAAAAAAAAAAAA",
                        {L"AAAAAAAAAAAAAAAAAAA", L"AAAAAAAAAAAAAAAAAAA2"}),
          L"AAAAAAAAAAAAAAAAAAA3", "variant skips a name another player holds");

    // At the cap the suffix DISPLACES stem characters instead of overflowing.
    const std::wstring capped = AssignAgainst(L"BBBBBBBBBBBBBBBBBBBB",
                                              {L"BBBBBBBBBBBBBBBBBBBB"});
    check(capped, L"BBBBBBBBBBBBBBBBBBB2", "suffix displaces the stem at the cap");
    ++total;
    if (capped.size() <= coop::player_handshake::kNickMaxChars) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- variant exceeded the cap");

    // A two-digit suffix displaces TWO characters. The taken set has to be built
    // the way the arbiter actually numbers -- the first drill asserted
    // C*19 + "10", which is 21 characters and therefore a name the arbiter can
    // never produce, so the test failed while the code was right.
    std::vector<std::wstring> many;
    many.push_back(std::wstring(20, L'C'));                                  // n = 1
    for (int n = 2; n <= 9; ++n)
        many.push_back(std::wstring(19, L'C') + std::to_wstring(n));         // n = 2..9
    many.push_back(std::wstring(18, L'C') + L"10");                          // n = 10
    const std::wstring twoDigit = AssignAgainst(std::wstring(20, L'C'), many);
    check(twoDigit, (std::wstring(18, L'C') + L"11").c_str(), "two-digit suffix at the cap");

    // No collision -> untouched.
    check(AssignAgainst(L"Someone", {L"Host", L"Other"}), L"Someone", "no collision");

    // --- ARC D2: the fold describes PIXELS, not strings -----------------------
    //
    // The case the whole arc exists for. Two all-hanzi names share no codepoint,
    // so the old string fold said "distinct" and neither took a suffix -- yet
    // ImGui draws BOTH as two identical fallback boxes. They must collide now.
    check(AssignAgainst(L"\x5F20\x4F1F", {L"\x674E\x660E"}), L"\x5F20\x4F1F\x32",
          "two distinct CJK names collide (they render alike)");
    // ...and the COMMON case stays clean: one such peer alone keeps a bare name.
    check(AssignAgainst(L"\x5F20\x4F1F", {L"Pelmentor"}), L"\x5F20\x4F1F",
          "a lone out-of-repertoire name takes no suffix");
    // Length still separates them, because N absent codepoints draw as N boxes.
    check(AssignAgainst(L"\x5F20\x4F1F\x660E", {L"\x674E\x660E"}), L"\x5F20\x4F1F\x660E",
          "different LENGTHS of absent text stay distinct (N boxes vs M)");
    // Mixed text sentinels only the part that cannot be drawn.
    ++total;
    if (FoldKey(L"a\x4E2D" L"b") == FoldKey(L"a\x674E" L"b") &&
        FoldKey(L"a\x4E2D" L"b") != FoldKey(L"a" L"cb")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- mixed in/out folding");
    // Cyrillic case folds now that Cyrillic is an alphabet we accept AND draw.
    check(AssignAgainst(L"\x41F\x415\x41B\x42C\x41C\x415\x41D\x42C",
                        {L"\x43F\x435\x43B\x44C\x43C\x435\x43D\x44C"}),
          L"\x41F\x415\x41B\x42C\x41C\x415\x41D\x42C\x32", "Cyrillic case folds");
    // An astral codepoint is ONE key element, not two. Folding units gave 𠀀 two
    // sentinels and 中 one, so a pair like this would not have collided.
    ++total;
    if (FoldKey(L"\xD840\xDC00") == FoldKey(L"\x4E2D")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- astral folds to ONE sentinel");
    // Emoji are IN the repertoire, so they are NOT sentinelled and stay distinct.
    ++total;
    if (FoldKey(L"\xD83D\xDE00") != FoldKey(L"\xD83D\xDE0D")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- two emoji collapsed together");
    // A LITERAL U+FFFD renders exactly like an absent codepoint, so it must fold
    // to the same key. Picking any other sentinel re-opens the whole defect one
    // level down, and it is the kind of hole nobody finds twice.
    ++total;
    if (FoldKey(L"\xFFFD") == FoldKey(L"\x4E2D")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- a literal U+FFFD is not the sentinel");

    // The cap truncates in CODEPOINTS: a 20-emoji name whose suffix displaces
    // the tail must not leave half a surrogate pair behind.
    {
        const std::wstring emoji20 = [] {
            std::wstring s;
            for (int i = 0; i < 20; ++i) s += L"\xD83D\xDE00";
            return s;
        }();
        const std::wstring got = AssignAgainst(emoji20, {emoji20});
        // The assertion that actually detects a split pair: ToUtf8 DROPS a lone
        // surrogate, so a UTF-8 round trip comes back SHORTER exactly when the
        // cap cut through one. Checking the length alone would not -- the old
        // substr produced a 20-"character" string that was still broken.
        const std::string u8 = coop::text::ToUtf8(got);
        std::wstring back;
        check(coop::text::FromUtf8Strict(u8.data(), u8.size(), &back) ? back : L"<ill-formed>",
              got.c_str(), "the cap keeps surrogate pairs whole");
        ++total;
        if (coop::text::CountCodepoints(got) == coop::player_handshake::kNickMaxChars) ++pass;
        else UE_LOGE("nickname-arbiter selftest: FAIL -- capped emoji name is not 20 codepoints");
    }

    UE_LOGI("nickname-arbiter selftest: %s (%d/%d)", pass == total ? "PASS" : "FAIL", pass, total);
    return pass == total;
}

}  // namespace coop::nickname_arbiter
