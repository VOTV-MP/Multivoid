// coop/dev/kdress_drill_client_arms.cpp -- the client's halves of the pat, born and join arms; see coop/dev/kdress_drill.h.
// The put-on and the take-off are kdress_drill_client.cpp.
//
// The pat: the client pats its copy as the radial does (actionName 'pat'), which asks the host; once its own montage has
// ended it latches every tick whether the montage plays on its copy, and the host's pat after it is read at the host's line.
// The born arm and the join: the client reads its copy of the Omega the host names when the copy stands (born) and once the
// host says its join send has gone (the join), and says what it wears.

#include "kdress_drill_detail.h"

#include "coop/player/players_registry.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/core/reflection.h"

#include <string>

namespace coop::dev::kdress_drill::detail {

namespace K  = ue_wrap::kerfur;
namespace KD = ue_wrap::kerfur_dress;
namespace R  = ue_wrap::reflection;

namespace {

int g_pat = 0;
bool g_patSeen = false;   // the pat montage played on this copy since it went idle after the client's own pat

int g_born = 0, g_join = 0, g_eid = 0;

void PatStep(int to) {
    g_pat = to;
    Go();
}

// The eid a host line of `prefix` names; false (and the bound) until it comes.
bool EidFrom(const char* prefix, const char* lateWhy) {
    std::string rest;
    if (!Rest(prefix, &rest)) {
        Late(lateWhy);
        return false;
    }
    if (!ParseInts(rest, 1, &g_eid)) {
        Invalid("the host's line could not be read");
        return false;
    }
    return true;
}

}  // namespace

void PatClient() {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    switch (g_pat) {
    case 0:
        if (!ClientBindC()) return;
        Say("c-ready");
        PatStep(1);
        return;
    case 1: {
        if (!Seen("go-a")) {
            Late("the host never said go");
            return;
        }
        void* const c = g_c.Get();
        if (!c) return Invalid("this client's copy of Omega C is gone");
        // The radial's pat on the client's own copy: the seat's watch asks the host.
        const bool ran = K::RunActionName(c, me, L"pat");
        Fact("actionName 'pat' on C ran=%d", ran ? 1 : 0);
        Say(std::string("c-pat-a ") + (ran ? "1" : "0"));
        if (!ran) return Finish();
        PatStep(2);
        return;
    }
    case 2: {   // idle again: whatever plays on this copy after this is the host's
        void* const c = g_c.Get();
        if (!c) return Invalid("this client's copy of Omega C is gone");
        if (KD::IsPatPlaying(c)) {
            Late("this client's own pat montage never ended");
            return;
        }
        g_patSeen = false;
        Say("c-idle-a");
        PatStep(3);
        return;
    }
    case 3: {
        void* const c = g_c.Get();
        if (!c) return Invalid("this client's copy of Omega C is gone");
        g_patSeen = g_patSeen || KD::IsPatPlaying(c);
        if (!Seen("h-pat-b")) {
            Late("the host never said its pat ran");
            return;
        }
        Fact("the host's pat: the montage %s on this copy of Omega C", g_patSeen ? "played" : "did not play");
        Say(std::string("c-b ") + (g_patSeen ? "1" : "0"));
        Finish();
        return;
    }
    }
}

void BornClient() {
    switch (g_born) {
    case 0:
        if (!ClientMayStart()) return;
        if (!EidFrom("born ", "the host never said Omega K is registered")) return;
        g_born = 1;
        Go();
        return;
    case 1: {   // the copy stands: what it wears at its bind
        void* const k = OmegaOf(g_eid);
        if (!k) {
            Late("this client's copy of Omega K never bound");
            return;
        }
        const int wears = DripHas(k, kRow) ? 1 : 0;
        Fact("Omega K eid=%d is bound here and wears [%s]", g_eid, DripText(k).c_str());
        Say("c-born " + std::to_string(wears));
        Finish();
        return;
    }
    }
}

void JoinClient() {
    switch (g_join) {
    case 0:
        if (!ClientMayStart()) return;
        if (!EidFrom("join-sent ", "the host never said its join send has gone")) return;
        g_join = 1;
        Go();
        return;
    case 1: {
        void* const j = OmegaOf(g_eid);
        if (!j) {
            Late("the joiner's copy of Omega J never bound");
            return;
        }
        const int wears = DripHas(j, kRow) ? 1 : 0;
        Fact("after the host's join send, Omega J eid=%d wears [%s]", g_eid, DripText(j).c_str());
        Say("c-join " + std::to_string(wears));
        Finish();
        return;
    }
    }
}

void ResetClientArms() {
    g_pat = 0;
    g_patSeen = false;
    g_born = g_join = g_eid = 0;
}

}  // namespace coop::dev::kdress_drill::detail
