// coop/dev/kdress_drill_host_arms.cpp -- the host's halves of the pat, born and join arms; see coop/dev/kdress_drill.h. The
// put-on and the take-off are kdress_drill_host.cpp.
//
// The pat is read twice, each time on the peer that was shown it, by a latch that watches the montage every tick from the
// moment its peer says it is idle, and read at the line the pattern's other peer says after its pat has gone out.
// The born arm and the join are read by the client, at its bind and after the host's join send, and said back in one line.

#include "kdress_drill_detail.h"

#include "coop/creatures/kerfur_dress_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/core/cached_obj_ref.h"

#include <string>

namespace coop::dev::kdress_drill::detail {

namespace K  = ue_wrap::kerfur;
namespace KD = ue_wrap::kerfur_dress;
namespace KS = coop::kerfur_dress_sync;

namespace {

constexpr uint8_t kSlot = 1;

// ---- the pat ----

int g_pat = 0;
bool g_patSeen = false;   // the montage was seen playing on the host's copy since the client's `go-a`

void PatStep(int to) {
    g_pat = to;
    Go();
}

// ---- the born arm ----

int g_born = 0, g_eidK = 0;
ue_wrap::CachedObjRef g_k;

// ---- the join ----

int g_join = 0, g_eidJ = 0;
ue_wrap::CachedObjRef g_j;

}  // namespace

void PatHost(coop::net::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    switch (g_pat) {
    case 0:
        if (!HostStandC(s)) return;
        Say("targets " + std::to_string(g_eidC));
        PatStep(1);
        return;
    case 1:
        if (!Seen("c-ready")) {
            Late("the client never said its copy of Omega C is here");
            return;
        }
        g_patSeen = false;
        Say("go-a");
        PatStep(2);
        return;
    case 2: {   // the client pats; its ask reaches the host before its line, and the montage plays on the host's copy
        void* const c = OmegaOf(g_eidC);
        if (!c) return Invalid("the host's copy of Omega C is gone");
        g_patSeen = g_patSeen || KD::IsPatPlaying(c);
        std::string rest;
        if (!Rest("c-pat-a ", &rest)) {
            Late("the client never said its pat ran");
            return;
        }
        int ran = 0;
        if (!ParseInts(rest, 1, &ran) || !ran) return Invalid("the client's pat on its copy did not run");
        if (!g_patSeen)
            return Fail("the host's copy of Omega eid=%d did not play the pat montage after the client's pat ask", g_eidC);
        Fact("the client's pat: the host's copy played the pat montage");
        PatStep(3);
        return;
    }
    case 3: {   // both copies idle before the host pats, so a montage later on the client's copy is the host's
        void* const c = OmegaOf(g_eidC);
        if (!c) return Invalid("the host's copy of Omega C is gone");
        if (KD::IsPatPlaying(c) || !Seen("c-idle-a")) {
            Late("the pat montage never ended on both copies");
            return;
        }
        if (!me) return;
        const bool ran = K::RunActionName(c, me, L"pat");
        Fact("the host's own actionName 'pat' on Omega C ran=%d", ran ? 1 : 0);
        if (!ran) return Invalid("the host's pat did not run");
        Say("h-pat-b");
        PatStep(4);
        return;
    }
    case 4: {
        std::string rest;
        if (!Rest("c-b ", &rest)) {
            Late("the client never said whether its copy played the host's pat");
            return;
        }
        int seen = 0;
        if (!ParseInts(rest, 1, &seen)) return Invalid("the client's pat line could not be read");
        if (!seen)
            return Fail("the client's copy of Omega eid=%d did not play the pat montage after the host's own pat", g_eidC);
        Pass("pat: the host's copy played the montage for the client's pat, and the client's copy played it for the host's pat");
        return;
    }
    }
}

void BornHost(coop::net::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    switch (g_born) {
    case 0: {
        if (!HostMayStart(s) || !me) return;
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
        if (!rp.valid() || !rp.actor()) {
            Late("the client's puppet never stood on the host");
            return;
        }
        if (!CatalogReady()) return;
        // The spawn registers the Omega inside its own call, so the put-on of this tick finds an element id: this arm reads
        // the updateDrip send and the hold at the client's bind. A kerfur prop carries no `drip` (prop_kerfurOmega holds a
        // type and a sentient flag), so the game's own turn-on makes a naked Omega and no route of the game dresses one
        // before it registers.
        g_k.Set(SpawnOmega(rp.actor(), kCAheadCm, 0.f));
        if (!g_k.Get()) return Invalid("the world refused the Omega's spawn");
        const int eidAtDress = OmegaEid(g_k.Get());
        if (!DressOmega(me, g_k.Get())) return;
        Fact("spawned Omega K and dressed it in one tick: it wears [%s]; its element id at that tick was %d",
             DripText(g_k.Get()).c_str(), eidAtDress);
        g_born = 1;
        Go();
        return;
    }
    case 1:
        g_eidK = OmegaEid(g_k.Get());
        if (!g_eidK) {
            Late("Omega K never got an element id");
            return;
        }
        Say("born " + std::to_string(g_eidK));
        Fact("Omega K eid=%d is registered; said to the client", g_eidK);
        g_born = 2;
        Go();
        return;
    case 2: {
        std::string rest;
        if (!Rest("c-born ", &rest)) {
            Late("the client never read its copy of Omega K");
            return;
        }
        int wears = 0;
        if (!ParseInts(rest, 1, &wears)) return Invalid("the client's born line could not be read");
        if (!wears)
            return Fail("the client's copy of Omega eid=%d does not wear '%ls' at its bind: the host dressed it in the tick "
                        "it spawned it, and its updateDrip word did not dress the copy", g_eidK, kRow);
        Pass("born: the client's copy of an Omega the host dressed in the tick it spawned it wears the row at its bind");
        return;
    }
    }
}

void JoinHost(coop::net::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    switch (g_join) {
    case 0:
        if (!me || !CatalogReady()) return;
        g_j.Set(SpawnOmega(me, kJAheadCm, 0.f));
        if (!g_j.Get()) return Invalid("the world refused the Omega's spawn");
        Fact("spawned Omega J %.0f cm before the host's player", kJAheadCm);
        g_join = 1;
        Go();
        return;
    case 1:
        g_eidJ = OmegaEid(g_j.Get());
        if (!g_eidJ || !AtRest(g_j.Get())) {
            Late("Omega J never got an element id and came to rest");
            return;
        }
        g_join = 2;
        Go();
        return;
    case 2:
        if (!me || !DressOmega(me, g_j.Get())) return;
        Fact("J dressed: eid=%d wears [%s]; the client may be launched", g_eidJ, DripText(g_j.Get()).c_str());
        g_join = 3;
        Go();
        return;
    case 3: {   // no bound of this drill's: the run's budget is the joiner's
        // The joiner's world is captured and not yet ready: what it holds of J is the dressed one.
        if (!s.HasPeerConn(kSlot) || !coop::join_window_baseline::HasCapture(kSlot) || s.IsSlotWorldReady(kSlot)) return;
        KD::Unequip(g_j.Get(), kRow);
        if (DripHas(g_j.Get(), kRow)) return Invalid("the host's take-off did not undress J");
        Fact("undressed after capture: J eid=%d wears [%s]", g_eidJ, DripText(g_j.Get()).c_str());
        g_join = 4;
        Go();
        return;
    }
    case 4:
        if (!HostMayStart(s)) return;
        Say("join-sent " + std::to_string(g_eidJ));
        Fact("the joiner is world-ready and has the host's join send; said to the client");
        g_join = 5;
        Go();
        return;
    case 5: {
        std::string rest;
        if (!Rest("c-join ", &rest)) {
            Late("the client never read its copy of Omega J");
            return;
        }
        int wears = 0;
        if (!ParseInts(rest, 1, &wears)) return Invalid("the client's join line could not be read");
        if (!wears && KS::dev::RedOf() == KS::dev::Red::Snapshot)
            return Invalid("the red cannot fail: the joiner's copy of Omega J lacks the row without the host's join send");
        if (wears)
            return Fail("the joiner's copy of Omega eid=%d wears '%ls' the host took off after the capture: the host's join "
                        "send did not tell it", g_eidJ, kRow);
        Pass("join: the joiner's copy of an Omega the host undressed after the capture lacks the row once the host's join send "
             "is told");
        return;
    }
    }
}

void ResetHostArms() {
    g_pat = 0;
    g_patSeen = false;
    g_born = g_eidK = 0;
    g_k.Reset();
    g_join = g_eidJ = 0;
    g_j.Reset();
}

}  // namespace coop::dev::kdress_drill::detail
