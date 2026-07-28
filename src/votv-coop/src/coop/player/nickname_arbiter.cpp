// coop/player/nickname_arbiter.cpp -- see coop/player/nickname_arbiter.h.

#include "coop/player/nickname_arbiter.h"

#include "coop/player/roster_ledger.h"
#include "coop/session/player_handshake.h"
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
std::wstring Candidate(const std::wstring& stem, int n) {
    if (n <= 1) return stem;
    const std::wstring suffix = std::to_wstring(n);
    if (stem.size() + suffix.size() <= coop::player_handshake::kNickMaxChars)
        return stem + suffix;
    const size_t keep = (coop::player_handshake::kNickMaxChars > suffix.size())
                            ? coop::player_handshake::kNickMaxChars - suffix.size()
                            : 0;
    return stem.substr(0, keep) + suffix;
}

}  // namespace

std::wstring FoldKey(const std::wstring& name) {
    std::wstring key;
    key.reserve(name.size());
    for (wchar_t c : name)
        key.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
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

    UE_LOGI("nickname-arbiter selftest: %s (%d/%d)", pass == total ? "PASS" : "FAIL", pass, total);
    return pass == total;
}

}  // namespace coop::nickname_arbiter
