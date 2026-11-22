// coop/dev/bucket_drill_tick.cpp -- the pin, visuals and mop arms of the bucket drill; see coop/dev/bucket_drill.h.
//
// pin: the client writes its own copy's water down 2 cm (a local divergence the host never sees); at the copy's next tick the lane
// puts the host's row back before the tick runs, and the drill's POST watch reads the water the tick left. visuals: the host dips a
// bucket (its own call); the row must leave the copy's drawn water (height_interp) at least 0.1 cm from its water at the next tick's
// entry (else INVALID: nothing to draw), and a tick POST then finds the drawn water on the water with `empty` following it (PASS). A
// client whose plain tick is refused (the red `tick`) has no POST: the 20th refused tick after the row is the event at which the copy
// is read directly, and it FAILs when the drawn water still stands off the water. mop: the host dips the base's mop bucket and then a plain bucket; at the plain bucket's row past the dip the
// client's mop copy reads the host's mop water (the red `exact` tells only the plain class).

#include "bucket_drill_detail.h"

#include "coop/net/session.h"
#include "coop/props/bucket_sync.h"

#include "ue_wrap/actors/bucket.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace coop::dev::bucket_drill::detail {

namespace B  = ue_wrap::bucket;
namespace BS = coop::bucket_sync;

namespace {

constexpr unsigned long long kRefusedTicks = 20;   // refused ticks after the row that tell a frozen water mesh
constexpr float kGapEps = 0.1f;                    // the drawn water is on the water within this, in cm
// What the client says of its copy's draw after the row: the first word of its `c-r1` line.
constexpr int kRowMovedNothing = 0, kClosedAtTick = 1, kOffAtRefusals = 2, kOnAtRefusals = 3;

struct Tick {
    bool begun = false;
    int ph = 0, cph = 0;
    unsigned long long refusedBase = 0;
    int hostMop = 0;
} g_t;

bool Start(coop::net::Session& s, int plains, bool mop, const wchar_t* item, int& ph) {
    if (ph != 0) return true;
    if (!g_t.begun) {
        Spec sp;
        sp.plains = plains;
        sp.mop = mop;
        if (item) {
            sp.items[0] = item;
            sp.nItems = 1;
        }
        StageBegin(sp);
        g_t.begun = true;
    }
    if (!StageStep(s)) return false;
    ph = 1;
    Go();
    return true;
}

// The host's own dip of `bucket` with the staged sponge: the game's dipped, on the host's own state.
bool HostDip(void* bucket) {
    bool dipped = false;
    return B::CallDipped(bucket, StageItem(0), dipped) && dipped;
}

}  // namespace

// ================================================================ pin =====================================================

void PinHost(coop::net::Session& s) {
    Tick& a = g_t;
    if (a.ph == 0 && !Start(s, 1, false, nullptr, a.ph)) return;
    switch (a.ph) {
    case 1:
        if (!StageSay()) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        if (!Seen("c-ready")) {
            Late("the client never said its watch is live");
            return;
        }
        void* const b = StageBucket(0);
        B::Plain p;
        if (!b || !B::ReadPlain(b, p)) return Invalid("the host's bucket cannot be read");
        const uint32_t seq = BS::MarkDirty(b);
        if (!seq) return Invalid("the host's row of the bucket could not be marked");
        Say("seq1 " + Ints({EidOf(b), static_cast<int>(seq), Centi(p.height)}));
        a.ph = 3;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy read at its tick");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        if (!v[0])
            return Fail("the client's copy reads %.2f cm at its next tick after a local change, not put back to the host's row", static_cast<double>(v[1]) / 100.0);
        return Pass("pin: a client copy whose water was changed locally read the host's row again at its next tick");
    }
    }
}

void PinClient() {
    Tick& a = g_t;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(1, 0) || !WatchSettled()) return;
        Say("c-ready");
        a.cph = 1;
        Go();
        return;
    case 1: {
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v)) return;
        void* const copy = ClientBucket(0);
        B::Plain p;
        if (!copy || !B::ReadPlain(copy, p)) return Invalid("this client's copy cannot be read");
        // The copy's own water changes as a dip would, locally: nothing is asked and nothing reaches the host.
        if (!B::WritePlain(copy, p.height - 2.f, p.soap, p.alcoholP, p.alcohol, p.alcReady)) return Invalid("the copy's water could not be written");
        WatchArm(copy, static_cast<float>(v[2]) / 100.f, false);
        Fact("pin: the copy's water %.2f cm was written %.2f cm; the host's row says %.2f cm", static_cast<double>(p.height),
             static_cast<double>(p.height - 2.f), static_cast<double>(v[2]) / 100.0);
        a.cph = 2;
        Go();
        return;
    }
    default: {
        const TickSample t = WatchRead();
        if (t.posts == 0) {
            Late("the copy never ticked after the local change");
            return;
        }
        Say("c-r1 " + Ints({t.good ? 1 : 0, Centi(t.height)}));
        Finish();
        return;
    }
    }
}

// ================================================================ visuals =================================================

void VisualsHost(coop::net::Session& s) {
    Tick& a = g_t;
    if (a.ph == 0 && !Start(s, 1, false, L"prop_sponge", a.ph)) return;
    switch (a.ph) {
    case 1:
        if (!StageSay()) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        if (!Seen("c-ready")) {
            Late("the client never said its watch is live");
            return;
        }
        void* const b = StageBucket(0);
        B::Plain p;
        if (!b || !HostDip(b) || !B::ReadPlain(b, p)) return Invalid("the host's own dip of its bucket did not run");
        const uint32_t seq = BS::MarkDirty(b);
        if (!seq) return Invalid("the host's row of the bucket could not be marked");
        Say("seq1 " + Ints({EidOf(b), static_cast<int>(seq), Centi(p.height)}));
        a.ph = 3;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[4] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy drew");
            return;
        }
        if (!ParseInts(rest, 4, v)) return Invalid("the client's reading could not be read");
        const double water = static_cast<double>(v[1]) / 100.0, drawn = static_cast<double>(v[2]) / 100.0, gap0 = static_cast<double>(v[3]) / 100.0;
        char why[200];
        switch (v[0]) {
        case kRowMovedNothing:
            std::snprintf(why, sizeof(why), "the host's dip moved nothing to draw: the client copy's drawn water stood within 0.1 cm of its water at "
                          "the row (a gap of %.2f cm)", gap0);
            return Invalid(why);
        case kClosedAtTick:
            return Pass("visuals: after the host's dip the client copy's drawn water reached the water and its empty flag followed it, at a "
                        "tick after the row that had moved the drawn water");
        case kOffAtRefusals:
            return Fail("the client's copy draws its water at %.2f cm while the water stands at %.2f cm after %llu refused ticks (the row left a gap of "
                        "%.2f cm): its own tick never moved the drawn water", drawn, water, kRefusedTicks, gap0);
        default:
            std::snprintf(why, sizeof(why), "the client copy's drawn water stands on its water after %llu refused ticks with no tick: the refusal "
                          "froze nothing", kRefusedTicks);
            return Invalid(why);
        }
    }
    }
}

void VisualsClient() {
    Tick& a = g_t;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(1, 1) || !WatchSettled()) return;
        WatchArm(ClientBucket(0), 0.f, true, static_cast<uint32_t>(ClientEid(0)));   // before the row: its first tick entry is noted
        Say("c-ready");
        a.cph = 1;
        Go();
        return;
    case 1: {
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v)) return;
        WatchTarget(static_cast<uint32_t>(v[1]));
        a.refusedBase = BS::CountOf(BS::Count::PlainTicksRefused);
        a.cph = 2;
        Go();
        return;
    }
    default: {
        const TickSample t = WatchRead();
        if (t.gap0 < 0.f) {
            Late("the copy never ticked after the row");
            return;
        }
        const unsigned long long refused = BS::CountOf(BS::Count::PlainTicksRefused) - a.refusedBase;
        const int gap0 = Centi(t.gap0);
        if (t.gap0 < kGapEps) {   // the row moved nothing to draw: neither verdict can be read from it
            Say("c-r1 " + Ints({kRowMovedNothing, 0, 0, gap0}));
            Finish();
            return;
        }
        if (t.good) {   // a tick POST after the row found the drawn water on the water
            Say("c-r1 " + Ints({kClosedAtTick, Centi(t.height), Centi(t.interp), gap0}));
            Finish();
            return;
        }
        if (refused < kRefusedTicks) {
            Late("the drawn water never closed on the water after the row, and the refused ticks never reached the count");
            return;
        }
        // The 20th refused tick: no POST will come, so the copy is read where it stands.
        B::Plain p;
        if (!B::ReadPlain(ClientBucket(0), p)) return Invalid("this client's copy cannot be read");
        const bool on = std::fabs(p.heightInterp - p.height) < kGapEps && p.empty == (p.height <= 0.f);
        Say("c-r1 " + Ints({on ? kOnAtRefusals : kOffAtRefusals, Centi(p.height), Centi(p.heightInterp), gap0}));
        Finish();
        return;
    }
    }
}

// ================================================================ mop =====================================================

void MopHost(coop::net::Session& s) {
    Tick& a = g_t;
    if (a.ph == 0 && !Start(s, 2, true, L"prop_sponge", a.ph)) return;
    switch (a.ph) {
    case 1:
        if (!StageSay()) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        if (!Seen("c-ready")) {
            Late("the client never said its copies are bound");
            return;
        }
        void* const mop = StageBucket(0);
        void* const plain = StageBucket(1);
        B::Plain pm, pp;
        // The host's own dips: the mop bucket first, then the plain one, whose row is the one the client waits for.
        if (!mop || !plain || !HostDip(mop) || !B::ReadPlain(mop, pm) || !BS::MarkDirty(mop))
            return Invalid("the host's own dip of the mop bucket did not run");
        a.hostMop = Centi(pm.height);
        if (!HostDip(plain) || !B::ReadPlain(plain, pp)) return Invalid("the host's own dip of the plain bucket did not run");
        const uint32_t seq = BS::MarkDirty(plain);
        if (!seq) return Invalid("the host's row of the plain bucket could not be marked");
        Fact("mop: the host's mop bucket reads %.2f cm, the plain one %.2f cm", static_cast<double>(pm.height), static_cast<double>(pp.height));
        Say("seq1 " + Ints({EidOf(plain), static_cast<int>(seq), a.hostMop}));
        a.ph = 3;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[1] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its mop copy read");
            return;
        }
        if (!ParseInts(rest, 1, v)) return Invalid("the client's reading could not be read");
        if (v[0] != a.hostMop)
            return Fail("the client's mop copy reads %.2f cm at the plain bucket's row past the dip, the host's mop bucket reads %.2f cm",
                        static_cast<double>(v[0]) / 100.0, static_cast<double>(a.hostMop) / 100.0);
        return Pass("mop: the host's dip of the mop bucket reached the client's mop copy by the time the plain bucket's row past it did");
    }
    }
}

void MopClient() {
    Tick& a = g_t;
    if (!Local()) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(2, 1)) return;
        Say("c-ready");
        a.cph = 1;
        Go();
        return;
    default: {
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v)) return;
        B::Plain p;
        if (!B::ReadPlain(ClientBucket(0), p)) return Invalid("this client's mop copy cannot be read");
        Say("c-r1 " + Ints({Centi(p.height)}));
        Finish();
        return;
    }
    }
}

void ResetTickArms() { g_t = Tick{}; }

}  // namespace coop::dev::bucket_drill::detail
