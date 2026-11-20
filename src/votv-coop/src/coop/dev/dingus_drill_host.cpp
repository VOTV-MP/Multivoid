// coop/dev/dingus_drill_host.cpp -- the host's steps of the dingus drill: it spawns the targets, reads its own copies where the
// lane reads them (the birth drain), grabs and holds a dingus of its own where an arm needs the host to be the finder, and says
// the verdict; see coop/dev/dingus_drill.h. The log, chat and verbs are dingus_drill.cpp.

#include "dingus_drill_detail.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/dingus_sync.h"

#include "ue_wrap/actors/dingus.h"
#include "ue_wrap/core/reflection.h"

#include <cstdio>
#include <string>

namespace coop::dev::dingus_drill::detail {

namespace R  = ue_wrap::reflection;
namespace D  = ue_wrap::dingus;
namespace DS = coop::dingus_sync::dev;

namespace {

constexpr float kAheadCm = 90.f, kSideCm = 40.f;   // a target before the client's puppet stands this far ahead and to a side
constexpr float kNearCm = 110.f;                   // a target before the host's own player, within the use key's reach
constexpr const wchar_t* kBase = L"prop_dingus_C";
constexpr const wchar_t* kSub = L"prop_dingus2_C";

enum class HS {
    Start, Settle, WaitReady,
    FindWaitA, FindGrabC, FindWaitReadC,
    PocketWaitBornB, PocketTakeB, PocketWaitBornB2, PocketWaitBoundB, PocketGrabD, PocketTake, PocketWaitBornD, PocketWaitBoundD,
    LoadedWaitBorn,
    JoinGrab, JoinWaitJoiner, JoinTake, JoinWaitBorn, JoinWaitBound
};
HS g_h = HS::Start;
ue_wrap::CachedObjRef g_t[2];   // find: A, C. pocket: B, D. loaded: E. join: F
int g_eid[2] = {0, 0};
std::wstring g_key[2];
GrabRun g_grab;
PocketRun g_pocket;
uint32_t g_judged0 = 0;
int g_bornEid = 0;   // pocket: the element id of the copy the host born from the client's drop

void HGo(HS s) {
    g_h = s;
    Go();
}

int TargetCount() { return ArmOf() == Arm::Find || ArmOf() == Arm::Pocket ? 2 : 1; }

bool ReadPair(const char* prefix, int& a, int& b) {
    std::string r;
    int v[2] = {0, 0};
    if (!Rest(prefix, &r) || !ParseInts(r, 2, v)) return false;
    a = v[0];
    b = v[1];
    return true;
}

// The targets: near the client's puppet where the client acts on them, before the host's own player where the host does.
bool SpawnTargets(void* me, void* puppet) {
    switch (ArmOf()) {
    case Arm::Find:
        g_t[0].Set(SpawnDingus(puppet, kAheadCm, -kSideCm, kSub));   // a subclass: the by-name watch is proven past the base class
        g_t[1].Set(SpawnDingus(me, kNearCm, 0.f, kBase));
        break;
    case Arm::Pocket:
        g_t[0].Set(SpawnDingus(puppet, kAheadCm, kSideCm, kBase));
        g_t[1].Set(SpawnDingus(me, kNearCm, 0.f, kBase));
        break;
    case Arm::Loaded: {
        // The game's own load spawns an actor and runs `loadData` on it, which marks a found dingus as the lane's marking does.
        void* const e = SpawnDingus(puppet, kAheadCm, 0.f, kBase);
        g_t[0].Set(e);
        if (e && !D::MarkFound(e)) {
            Invalid("the loaded dingus could not be marked found");
            return false;
        }
        break;
    }
    case Arm::Join:
        g_t[0].Set(SpawnDingus(me, kNearCm, 0.f, kBase));
        break;
    default:
        return false;
    }
    return g_t[0].Get() && (TargetCount() == 1 || g_t[1].Get());
}

// True when a copy reads found and quiet. Otherwise the verdict is said: a read the instrument could not take (-1) is INVALID,
// any other pair is a FAIL.
bool QuietOrEnd(int found, int music, const char* what) {
    const Reading r = ReadingOf(found, music);
    if (r == Reading::Quiet) return true;
    if (r == Reading::Unread) Invalid((std::string(what) + " could not be read (isUsed " + std::to_string(found) + ", music " +
                                       std::to_string(music) + ")").c_str());
    else Fail("%s reads isUsed=%d music=%d", what, found, music);
    return false;
}

void Verdict2(int found, int music, const char* what, const char* okWhy) {
    if (QuietOrEnd(found, music, what)) Pass(okWhy);
}

void HostStep(coop::net::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    const Arm arm = ArmOf();
    switch (g_h) {
    case HS::Start: {
        void* puppet = nullptr;
        if (arm != Arm::Join) {
            if (!HostMayStart(s)) return;
            coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
            if (!rp.valid() || !rp.actor()) {
                Late("the client's puppet never stood on the host");
                return;
            }
            puppet = rp.actor();
        }
        LogPlaced();
        if (!SpawnTargets(me, puppet)) {
            if (!Ended()) Invalid("the dingus class did not resolve or the world refused a spawn");
            return;
        }
        Fact("spawned the arm's dingi (%d)", TargetCount());
        HGo(HS::Settle);
        return;
    }
    case HS::Settle: {
        void* t[2] = {g_t[0].Get(), g_t[1].Get()};
        const int n = TargetCount();
        for (int i = 0; i < n; ++i)
            if (!t[i]) return Invalid("a dingus died before the drill began");
        for (int i = 0; i < n; ++i) g_eid[i] = EidOf(t[i]);
        if (!g_eid[0] || (n == 2 && !g_eid[1]) || !Quiet(t, n)) {
            Late("the dingi never got element ids and came to rest");
            return;
        }
        for (int i = 0; i < n; ++i) {
            g_key[i] = KeyOf(t[i]);
            if (g_key[i].empty() || g_key[i] == L"None") return Invalid("a dingus has no key");
        }
        if (arm == Arm::Loaded) {
            // The birth callback only queues: the drain reads the loaded copy a tick later and puts its key in the set.
            const Rec* r = FindBorn(g_key[0], 0);
            if (!r) {
                Late("the birth drain never read the loaded dingus");
                return;
            }
            if (r->found != 1) return Invalid("the loaded dingus did not read found at the birth drain");
            Fact("the drain read the loaded dingus '%ls' found (eid %d); the found set holds it: %d", g_key[0].c_str(), r->eid,
                 DS::FoundHas(g_key[0]) ? 1 : 0);
        }
        if (arm == Arm::Join) {
            g_grab.Begin(me, t[0]);
            HGo(HS::JoinGrab);
            return;
        }
        char line[80];
        std::snprintf(line, sizeof(line), n == 2 ? "targets %d %d" : "targets %d", g_eid[0], g_eid[1]);
        Say(line);
        Fact("the dingi at rest: eids %d %d, keys '%ls' '%ls'; said to the client", g_eid[0], g_eid[1], g_key[0].c_str(), g_key[1].c_str());
        HGo(HS::WaitReady);
        return;
    }
    case HS::WaitReady:
        if (!Seen("c-ready")) {
            Late("the client never said its copies are here");
            return;
        }
        if (arm == Arm::Find) {
            Say("find-go");
            HGo(HS::FindWaitA);
        } else if (arm == Arm::Pocket) {
            Say("pocket-go");
            HGo(HS::PocketWaitBornB);
        } else {
            g_judged0 = DS::Judge().judged;
            Say("loaded-go");
            HGo(HS::LoadedWaitBorn);
        }
        return;

    // ---- find ----
    case HS::FindWaitA: {
        if (!Seen("c-found A")) {
            Late("the client never said it found A");
            return;
        }
        void* const a = g_t[0].Get();
        if (!a) return Invalid("the host's copy of A is gone");
        Fact("find (a): the host's copy of A reads isUsed=%d music=%d", D::ReadFound(a), D::ReadMusicOn(a));
        if (!QuietOrEnd(D::ReadFound(a), D::ReadMusicOn(a), "find (a): the host's copy of A after the client's find, so the report did not reach it,"))
            return;
        g_grab.Begin(me, g_t[1].Get());
        HGo(HS::FindGrabC);
        return;
    }
    case HS::FindGrabC: {
        const GrabRun::R r = g_grab.Tick();
        if (r == GrabRun::R::Working) {
            Late("the host's aimed grab never ended");
            return;
        }
        if (r == GrabRun::R::Failed) return Invalid(g_grab.Why());
        Say("h-found C");
        HGo(HS::FindWaitReadC);
        return;
    }
    case HS::FindWaitReadC: {
        int f = -1, m = -1;
        if (!ReadPair("c-read C ", f, m)) {
            Late("the client never said what its copy of C reads");
            return;
        }
        Verdict2(f, m, "find (b): the client's copy of C after the host's find", "the client's find reached the host's copy and the "
                 "host's find reached the client's");
        return;
    }

    // ---- pocket ----
    case HS::PocketWaitBornB: {
        const Rec* r = FindBorn(g_key[0], g_eid[0]);
        if (!r) {
            Late("the host never births a copy of B's key from the client's drop");
            return;
        }
        if (!QuietOrEnd(r->found, r->music, "pocket: the host's copy born from the client's drop of the found dingus, at its birth drain,"))
            return;
        // The host holds that copy and drops it: the copy the client's mirror binds is the game's own, and the client's set holds
        // the key only from the host's word.
        void* const born = LiveProp(r->eid);
        if (!born) return Invalid("the host's copy born from the client's drop is gone before the host's pocket verb");
        g_bornEid = r->eid;
        g_pocket.Begin(me, born, false);
        HGo(HS::PocketTakeB);
        return;
    }
    case HS::PocketTakeB: {
        const PocketRun::R r = g_pocket.Tick();
        if (r == PocketRun::R::Working) {
            Late("the host's pocket and drop of the client's drop-born copy never ended");
            return;
        }
        if (r == PocketRun::R::Failed) return Invalid(g_pocket.Why());
        HGo(HS::PocketWaitBornB2);
        return;
    }
    case HS::PocketWaitBornB2: {
        const Rec* r = FindBorn(g_key[0], g_eid[0], g_bornEid);
        if (!r) {
            Late("the host never births a copy of B's key from its own drop");
            return;
        }
        Say("b-dropped " + std::to_string(r->eid));
        HGo(HS::PocketWaitBoundB);
        return;
    }
    case HS::PocketWaitBoundB: {
        int f = -1, m = -1;
        if (!ReadPair("c-bound B ", f, m)) {
            Late("the client never said what its mirror of the host's drop of B reads");
            return;
        }
        if (!QuietOrEnd(f, m, "pocket: the client's mirror of the host's drop of the dingus the client found, at its bind,")) return;
        g_grab.Begin(me, g_t[1].Get());
        HGo(HS::PocketGrabD);
        return;
    }
    case HS::PocketGrabD: {
        const GrabRun::R r = g_grab.Tick();
        if (r == GrabRun::R::Working) {
            Late("the host's aimed grab never ended");
            return;
        }
        if (r == GrabRun::R::Failed) return Invalid(g_grab.Why());
        g_pocket.Begin(me, g_t[1].Get(), false);
        HGo(HS::PocketTake);
        return;
    }
    case HS::PocketTake: {
        const PocketRun::R r = g_pocket.Tick();
        if (r == PocketRun::R::Working) {
            Late("the host's pocket and drop never ended");
            return;
        }
        if (r == PocketRun::R::Failed) return Invalid(g_pocket.Why());
        HGo(HS::PocketWaitBornD);
        return;
    }
    case HS::PocketWaitBornD: {
        const Rec* r = FindBorn(g_key[1], g_eid[1]);
        if (!r) {
            Late("the host never births a copy of D's key from its own drop");
            return;
        }
        Say("h-dropped " + std::to_string(r->eid));
        HGo(HS::PocketWaitBoundD);
        return;
    }
    case HS::PocketWaitBoundD: {
        int f = -1, m = -1;
        if (!ReadPair("c-bound D ", f, m)) {
            Late("the client never said what its mirror of the host's drop reads");
            return;
        }
        Verdict2(f, m, "pocket: the client's mirror of the host's drop of the found dingus at its bind",
                 "a copy born from a drop of a found dingus is found on the host at its birth and on the client at its bind");
        return;
    }

    // ---- loaded ----
    case HS::LoadedWaitBorn: {
        const Rec* r = FindBorn(g_key[0], g_eid[0]);
        if (!r) {
            Late("the host never births a copy of E's key from the client's drop");
            return;
        }
        if (DS::Judge().judged != g_judged0)
            return Invalid("a report was judged during the arm: the client's hold ran a touch, so a report could have added the key");
        Verdict2(r->found, r->music, "loaded: the host's copy born from the client's drop of the dingus its world loaded found",
                 "the birth drain seeded the found set from a dingus loaded found, and its drop-born copy is found");
        return;
    }

    // ---- join ----
    case HS::JoinGrab: {
        const GrabRun::R r = g_grab.Tick();
        if (r == GrabRun::R::Working) {
            Late("the host's aimed grab never ended");
            return;
        }
        if (r == GrabRun::R::Failed) return Invalid(g_grab.Why());
        if (!DS::FoundHas(g_key[0])) return Fail("join: the host's own find of F did not put its key in the found set");
        Fact("join dingus found: '%ls' (eid %d) is in the host's set; the client may be launched", g_key[0].c_str(), g_eid[0]);
        HGo(HS::JoinWaitJoiner);
        return;
    }
    case HS::JoinWaitJoiner:   // the joiner's own wait has no bound of this drill's: the run's budget is its bound
        if (!HostMayStart(s) || !Seen("c-ready")) return;
        g_pocket.Begin(me, g_t[0].Get(), false);
        HGo(HS::JoinTake);
        return;
    case HS::JoinTake: {
        const PocketRun::R r = g_pocket.Tick();
        if (r == PocketRun::R::Working) {
            Late("the host's pocket and drop never ended");
            return;
        }
        if (r == PocketRun::R::Failed) return Invalid(g_pocket.Why());
        HGo(HS::JoinWaitBorn);
        return;
    }
    case HS::JoinWaitBorn: {
        const Rec* r = FindBorn(g_key[0], g_eid[0]);
        if (!r) {
            Late("the host never births a copy of F's key from its own drop");
            return;
        }
        Say("h-dropped " + std::to_string(r->eid));
        HGo(HS::JoinWaitBound);
        return;
    }
    case HS::JoinWaitBound: {
        int f = -1, m = -1;
        if (!ReadPair("c-bound F ", f, m)) {
            Late("the joiner never said what its mirror of the host's drop reads");
            return;
        }
        Verdict2(f, m, "join: the joiner's mirror of the host's drop of F at its bind",
                 "a dingus found before a client joined is found on the joiner's copy born after the join");
        return;
    }
    }
}

}  // namespace

void HostTick(coop::net::Session& s) { HostStep(s); }

void ResetHost() {
    g_h = HS::Start;
    g_t[0].Reset();
    g_t[1].Reset();
    g_eid[0] = g_eid[1] = 0;
    g_key[0].clear();
    g_key[1].clear();
    g_judged0 = 0;
    g_bornEid = 0;
}

}  // namespace coop::dev::dingus_drill::detail
