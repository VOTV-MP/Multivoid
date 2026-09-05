// coop/player/nickname_arbiter.cpp -- see coop/player/nickname_arbiter.h.

#include "coop/player/nickname_arbiter.h"

#include "coop/player/roster_ledger.h"
#include "coop/session/player_handshake.h"
#include "coop/text/case_fold.h"
#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <string>
#include <vector>

namespace coop::nickname_arbiter {
namespace {

// Every distinct display name we could produce for one stem, in the order the
// dense-smallest-free policy tries them; n of 1 is the bare stem. The cap displaces the stem,
// which is why uniqueness is checked on the result rather than on the stem: a 19-character
// name plus "2" is 20 characters and can equal a different player's 20-character name that
// ends in '2'. The stem is the whole requested name, trailing digits included, so a kept
// "Pelmentor2" that meets another becomes "Pelmentor22", not "Pelmentor3": nothing in the
// string says whether a trailing number is a suffix we once added or part of the chosen name.
// The cap is in codepoints: a unit truncation could split a surrogate pair, and the UTF-8
// egress drops the lone surrogate, so the arbiter would judge uniqueness on a string every
// egress emitted differently.
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

}  // namespace

std::wstring FoldKey(const std::wstring& name) {
    // The key describes what a human sees. It folds in codepoints: folding wchar_t units gave an
    // astral character two keys where a BMP character has one, so two characters drawn as the same
    // fallback box would not have collided. Every codepoint this build cannot draw folds to one
    // sentinel, so names that render identically collide and one of them takes the numeric suffix;
    // that makes "everyone has a unique nameplate" true on screen, and font-independently, since no
    // donor budget covers every script. One sentinel per codepoint, never a collapsed run: N absent
    // characters draw as N identical boxes, so a 2-character and a 3-character CJK name are
    // distinguishable and keep distinct keys. The sentinel is U+FFFD, the character an absent
    // codepoint actually draws as: ImGui picks the fallback glyph from U+FFFD, then '?', then a
    // space, and the repertoire bakes U+FFFD so the first choice always wins. Any other sentinel
    // would re-open the hole one level down, since a name containing a literal U+FFFD renders
    // exactly like an out-of-repertoire one. The key is not a stand-in for the pixels; it is the
    // pixels.
    std::wstring key;
    key.reserve(name.size());
    for (size_t i = 0; i < name.size(); ) {
        uint32_t cp = 0;
        const size_t units = coop::text::DecodeCodepoint(name, i, &cp);
        const wchar_t* at = name.data() + i;
        i += units;
        if (!coop::text::InRepertoire(cp)) { key.push_back(kAbsentSentinel); continue; }
        // The case table is generated from the same run as the repertoire, so the two cannot drift:
        // a hand-written table covering three scripts left most cased, drawable codepoints folding
        // to themselves, so two Greek spellings of one name did not collide.
        const uint32_t folded = coop::text::CaseFold(cp);
        // A fold may be astral (Deseret, Adlam), and truncating it to one wchar_t would corrupt the
        // key with nothing to say so.
        if (folded != cp) {
            if (folded <= 0xFFFF) {
                key.push_back(static_cast<wchar_t>(folded));
            } else {
                const uint32_t v = folded - 0x10000;
                key.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
                key.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
            }
        } else {
            key.append(at, units);
        }
    }
    return key;
}

std::wstring AssignAgainst(const std::wstring& requested,
                           const std::vector<std::wstring>& taken) {
    // Uniqueness is enforced over the set of names already assigned, never over a key recomputed
    // from this function's own output, which is not well-founded once a suffix displaces stem
    // characters at the cap.
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
    // The ledger's occupied rows are the collision set: the key is the nick store, not
    // mirror-element existence, since the two are not co-timed. Excluding our own row is what makes
    // this idempotent: a retried Join re-arbitrates the same request against the same others and
    // lands on the same answer instead of walking the suffix upward. Ghost-freeness is the ledger's
    // guarantee: its reconcile runs the death pass first and unconditionally, so a reconnecting
    // peer cannot collide with its own un-reaped row. That matters because the assigned name is
    // persisted, so a rename earned against a ghost would follow the player into every future
    // session.
    std::vector<std::wstring> taken;
    taken.reserve(coop::roster_ledger::kMaxSlots);
    for (int s = 0; s < coop::roster_ledger::kMaxSlots; ++s) {
        if (s == slot) continue;
        const coop::roster_ledger::Row& r = coop::roster_ledger::Get(s);
        if (r.occupied() && !r.nick.empty()) taken.push_back(r.nick);
    }
    const std::wstring assigned = AssignAgainst(requested, taken);
    // The host's record of the decision, logged on every arbitration, not only on a rename: "asked
    // X, got X, against N names" distinguishes nothing collided from the request never arrived,
    // since an empty request sanitises to the placeholder and then looks like an ordinary name.
    UE_LOGI("nickname_arbiter: slot %d asked '%ls' -> assigned '%ls' (vs %zu taken)",
            slot, requested.c_str(), assigned.c_str(), taken.size());
    return assigned;
}

// The selftest runs against the pure core, so it touches no ledger row, needs no game thread
// and can run at any point in boot. It covers what no LAN drill can stage on demand: the
// cap-displacing suffix, the 20-character collision a stem check would miss, case folding, and
// the kept-name cases.
bool RunNicknameArbiterSelftest() {
    int pass = 0, total = 0;
    auto check = [&](const std::wstring& got, const wchar_t* want, const char* what) {
        ++total;
        if (got == want) { ++pass; return; }
        UE_LOGE("nickname-arbiter selftest: FAIL %s -- got '%ls' want '%ls'", what,
                got.c_str(), want);
    };

    // The ask: three "Pelmentor" become Pelmentor, 2, 3.
    check(AssignAgainst(L"Pelmentor", {}), L"Pelmentor", "first Pelmentor");
    check(AssignAgainst(L"Pelmentor", {L"Pelmentor"}), L"Pelmentor2", "second Pelmentor");
    check(AssignAgainst(L"Pelmentor", {L"Pelmentor", L"Pelmentor2"}), L"Pelmentor3",
          "third Pelmentor");

    // Idempotent: re-arbitrating a name whose holder is excluded from the set returns the same
    // answer; a retried Join must not ratchet.
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor"}), L"Pelmentor2", "retry is stable");

    // The assigned name is kept, so a returning Pelmentor2 asks for Pelmentor2 and keeps it when
    // nobody else has it...
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor", L"Pelmentor3"}), L"Pelmentor2",
          "a kept name survives when free");
    // ...and the second Pelmentor2 is the one that moves. The whole requested name is the stem, so
    // it becomes Pelmentor22 rather than Pelmentor3.
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor2"}), L"Pelmentor22",
          "a second kept name suffixes the whole stem");
    check(AssignAgainst(L"Pelmentor2", {L"Pelmentor2", L"Pelmentor22"}), L"Pelmentor23",
          "and keeps walking");

    // Case folding: PELMENTOR collides with Pelmentor.
    check(AssignAgainst(L"PELMENTOR", {L"pelmentor"}), L"PELMENTOR2", "fold is case-insensitive");

    // Dense smallest-free: the freed number is reused by the next joiner.
    check(AssignAgainst(L"Pelmentor", {L"Pelmentor", L"Pelmentor3"}), L"Pelmentor2",
          "dense reuse of a freed number");

    // The cap trap a stem check would miss: the "+2" variant of a 19-character name is exactly the
    // 20-character name another player already holds.
    check(AssignAgainst(L"AAAAAAAAAAAAAAAAAAA",
                        {L"AAAAAAAAAAAAAAAAAAA", L"AAAAAAAAAAAAAAAAAAA2"}),
          L"AAAAAAAAAAAAAAAAAAA3", "variant skips a name another player holds");

    // At the cap the suffix displaces stem characters instead of overflowing.
    const std::wstring capped = AssignAgainst(L"BBBBBBBBBBBBBBBBBBBB",
                                              {L"BBBBBBBBBBBBBBBBBBBB"});
    check(capped, L"BBBBBBBBBBBBBBBBBBB2", "suffix displaces the stem at the cap");
    ++total;
    if (capped.size() <= coop::player_handshake::kNickMaxChars) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- variant exceeded the cap");

    // A two-digit suffix displaces two characters. The taken set is built the way the arbiter
    // numbers: a 19-C name plus "10" is 21 characters, a name the arbiter can never produce.
    std::vector<std::wstring> many;
    many.push_back(std::wstring(20, L'C'));                                  // n = 1
    for (int n = 2; n <= 9; ++n)
        many.push_back(std::wstring(19, L'C') + std::to_wstring(n));         // n = 2..9
    many.push_back(std::wstring(18, L'C') + L"10");                          // n = 10
    const std::wstring twoDigit = AssignAgainst(std::wstring(20, L'C'), many);
    check(twoDigit, (std::wstring(18, L'C') + L"11").c_str(), "two-digit suffix at the cap");

    // No collision: untouched.
    check(AssignAgainst(L"Someone", {L"Host", L"Other"}), L"Someone", "no collision");

    // The fold describes pixels, not strings. Two all-hanzi names share no codepoint, so a string
    // fold says distinct and neither takes a suffix, yet ImGui draws both as two identical fallback
    // boxes; they must collide.
    check(AssignAgainst(L"\x5F20\x4F1F", {L"\x674E\x660E"}), L"\x5F20\x4F1F\x32",
          "two distinct CJK names collide (they render alike)");
    // ...and the common case stays clean: one such peer alone keeps a bare name.
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
    // Cyrillic case folds; it is an alphabet we accept and draw.
    check(AssignAgainst(L"\x41F\x415\x41B\x42C\x41C\x415\x41D\x42C",
                        {L"\x43F\x435\x43B\x44C\x43C\x435\x43D\x44C"}),
          L"\x41F\x415\x41B\x42C\x41C\x415\x41D\x42C\x32", "Cyrillic case folds");
    // ...and the scripts a hand-written table would not fold: Greek case folds too.
    check(AssignAgainst(L"\x3A9\x3BC\x3AD\x3B3\x3B1", {L"\x3C9\x3BC\x3AD\x3B3\x3B1"}),
          L"\x3A9\x3BC\x3AD\x3B3\x3B1\x32", "GREEK case folds (the stale-table gap)");
    // An astral codepoint is one key element, not two: folding units gave an astral character two
    // sentinels and a BMP character one, so this pair would not have collided.
    ++total;
    if (FoldKey(L"\xD840\xDC00") == FoldKey(L"\x4E2D")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- astral folds to ONE sentinel");
    // Emoji are in the repertoire, so they are not sentinelled and stay distinct.
    ++total;
    if (FoldKey(L"\xD83D\xDE00") != FoldKey(L"\xD83D\xDE0D")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- two emoji collapsed together");
    // A literal U+FFFD renders exactly like an absent codepoint, so it must fold to the same key;
    // any other sentinel re-opens the defect one level down.
    ++total;
    if (FoldKey(L"\xFFFD") == FoldKey(L"\x4E2D")) ++pass;
    else UE_LOGE("nickname-arbiter selftest: FAIL -- a literal U+FFFD is not the sentinel");

    // The cap truncates in codepoints: a 20-emoji name whose suffix displaces the tail must not
    // leave half a surrogate pair behind.
    {
        const std::wstring emoji20 = [] {
            std::wstring s;
            for (int i = 0; i < 20; ++i) s += L"\xD83D\xDE00";
            return s;
        }();
        const std::wstring got = AssignAgainst(emoji20, {emoji20});
        // The assertion that detects a split pair: the UTF-8 encoder drops a lone surrogate, so a
        // round trip comes back shorter exactly when the cap cut through one; the length alone
        // would not show it.
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
