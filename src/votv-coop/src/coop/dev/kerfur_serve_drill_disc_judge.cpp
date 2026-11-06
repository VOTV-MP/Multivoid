// coop/dev/kerfur_serve_drill_disc_judge.cpp -- see coop/dev/kerfur_serve_drill_disc_judge.h.

#include "coop/dev/kerfur_serve_drill_disc_judge.h"

#include <cstdlib>

namespace coop::dev::kerfur_serve_drill::disc {
namespace {

// A field of a client line as a number, -1 when absent.
long long Num(const std::vector<std::string>& f, size_t i) {
    return f.size() > i && !f[i].empty() ? std::atoll(f[i].c_str()) : -1;
}

// The terms of a variant's expected result, collected: each failing one is named with its value.
struct Terms {
    std::string bad;
    void Need(bool ok, const std::string& what) {
        if (!ok) bad += (bad.empty() ? "" : "; ") + what;
    }
};
}  // namespace

// What the first press must have done, per variant: empty when every term holds, else the failing terms. consumed and
// consume-miss are the client's own counts of what the host's answer did to its disc, published the host's PropSpawn +
// PropDestroy count over the carry's span, carries the spans that built a copy since the snapshot.
std::string JudgeFirst(Variant v, const std::vector<std::string>& f, const Side& after, long long carries,
                       long long published) {
    Terms t;
    const long long consumed = Num(f, 4), misses = Num(f, 5), feedback = Num(f, 6);
    const std::string gone = f.size() > 0 ? f[0] : "?";
    const bool holds = after.hasRead && after.has;
    const auto n = [](long long v) { return std::to_string(v); };
    switch (v) {
    case Variant::Wh:
        t.Need(gone == "held", "client disc after=" + gone + " (want held)");
        t.Need(consumed == 0, "consumed=" + n(consumed) + " (want 0)");
        t.Need(feedback >= 1, "the game's hint was not shown on the client");
        t.Need(carries == 0, "carries=" + n(carries) + " (want 0)");
        t.Need(after.hasRead && !after.has, "host Omega hasFloppy after is not 0");
        break;
    case Variant::Swap:
        t.Need(consumed == 0, "consumed=" + n(consumed) + " (want 0)");
        t.Need(misses == 1, "consume-miss=" + n(misses) + " (want 1)");
        t.Need(carries == 1, "carries=" + n(carries) + " (want 1)");
        t.Need(holds, "host Omega hasFloppy after is not 1");
        t.Need(published == 0, "host-published=" + n(published) + " (want 0)");
        break;
    default:   // on, busy
        t.Need(gone == "gone", "client disc after=" + gone + " (want gone)");
        t.Need(consumed == 1, "consumed=" + n(consumed) + " (want 1)");
        t.Need(misses == 0, "consume-miss=" + n(misses) + " (want 0)");
        t.Need(carries == 1, "carries=" + n(carries) + " (want 1)");
        t.Need(holds, "host Omega hasFloppy after is not 1");
        t.Need(published == 0, "host-published=" + n(published) + " (want 0)");
        break;
    }
    return t.bad;
}

// The busy variant's second press: refused while the Omega is on its job, the second disc kept, no new copy.
std::string JudgeSecond(const std::vector<std::string>& f, const Side& after, long long carries, long long published) {
    Terms t;
    const std::string held = f.size() > 0 ? f[0] : "?";
    t.Need(held == "held", "second disc after=" + held + " (want held)");
    t.Need(Num(f, 1) >= 1, "no refusal was shown for the second press");
    t.Need(Num(f, 2) == 0, "the second press consumed a disc");
    t.Need(carries == 1, "carries=" + std::to_string(carries) + " after the second press (want 1)");
    t.Need(published == 0, "host-published=" + std::to_string(published) + " (want 0)");
    t.Need(after.hasRead && after.has, "host Omega hasFloppy after the second press is not 1");
    return t.bad;
}

}  // namespace coop::dev::kerfur_serve_drill::disc
