// coop/dev/dingus_drill_client.cpp -- the client's steps of the dingus drill: it grabs, holds and drops the dingi the host
// spawned near its puppet, reads its own copies where the lane reads them (its bind line), and says what they read; the host
// decides. See coop/dev/dingus_drill.h. The log, chat and verbs are dingus_drill.cpp.

#include "dingus_drill_detail.h"

#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"

#include "ue_wrap/actors/dingus.h"
#include "ue_wrap/core/reflection.h"

#include <cstdlib>
#include <string>

namespace coop::dev::dingus_drill::detail {

namespace R = ue_wrap::reflection;
namespace D = ue_wrap::dingus;
namespace PDS = coop::prop_drive_stream;

namespace {

enum class CS {
    WaitTargets, Settle, WaitGo,
    FindGrabA, FindWaitHost,
    PocketGrabB, PocketWaitDriveEnd, PocketTake, PocketWaitHostB, PocketWaitBoundB, PocketWaitHostD, PocketWaitBoundD,
    LoadedTake,
    JoinWaitDropped, JoinWaitBound
};
CS g_c = CS::WaitTargets;
ue_wrap::CachedObjRef g_t[2];   // find: A, C. pocket: B, D. loaded: E
int g_eid[2] = {0, 0};
std::wstring g_key[2];
GrabRun g_grab;
PocketRun g_pocket;
int g_dropEid = 0;   // the element id of the host's drop-born copy, as the host said it
// B's closed drive generation as it stood at the let-go (closed, generation): the drive of the release is over once it differs.
bool g_letGoClosed = false;
uint8_t g_letGoGen = 0;

void CGo(CS s) {
    g_c = s;
    Go();
}

int TargetCount() { return ArmOf() == Arm::Find || ArmOf() == Arm::Pocket ? 2 : 1; }

void SayBound(const char* label, const Rec& r) {
    Say(std::string("c-bound ") + label + " " + std::to_string(r.found) + " " + std::to_string(r.music));
}

void ClientStep() {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    const Arm arm = ArmOf();
    switch (g_c) {
    case CS::WaitTargets: {
        if (!ClientMayStart()) return;
        if (arm == Arm::Join) {
            LogPlaced();
            Say("c-ready");
            CGo(CS::JoinWaitDropped);
            return;
        }
        std::string rest;
        if (!Rest("targets ", &rest)) {
            Late("the host's targets line never came");
            return;
        }
        if (!ParseInts(rest, TargetCount(), g_eid)) return Invalid("the host's targets line could not be read");
        LogPlaced();
        CGo(CS::Settle);
        return;
    }
    case CS::Settle: {
        const int n = TargetCount();
        for (int i = 0; i < n; ++i) g_t[i].Set(LiveProp(g_eid[i]));
        void* const t[2] = {g_t[0].Get(), g_t[1].Get()};
        if (!t[0] || (n == 2 && !t[1]) || !Quiet(t, n)) {
            Late("the client never had the dingi at rest");
            return;
        }
        for (int i = 0; i < n; ++i) g_key[i] = KeyOf(t[i]);
        Fact("the dingi are here and at rest: eids %d %d, keys '%ls' '%ls'", g_eid[0], g_eid[1], g_key[0].c_str(), g_key[1].c_str());
        Say("c-ready");
        CGo(CS::WaitGo);
        return;
    }
    case CS::WaitGo:
        if (arm == Arm::Find) {
            if (!Seen("find-go")) { Late("the host never said find-go"); return; }
            g_grab.Begin(me, g_t[0].Get());
            CGo(CS::FindGrabA);
        } else if (arm == Arm::Pocket) {
            if (!Seen("pocket-go")) { Late("the host never said pocket-go"); return; }
            g_grab.Begin(me, g_t[0].Get());
            CGo(CS::PocketGrabB);
        } else {
            if (!Seen("loaded-go")) { Late("the host never said loaded-go"); return; }
            // The hold runs no `touch`, so no report can add the key: only the host's drain of the loaded copy can have.
            g_pocket.Begin(me, g_t[0].Get(), true);
            CGo(CS::LoadedTake);
        }
        return;

    // ---- find ----
    case CS::FindGrabA: {
        const GrabRun::R r = g_grab.Tick();
        if (r == GrabRun::R::Working) { Late("the aimed grab never ended"); return; }
        if (r == GrabRun::R::Failed) return Invalid(g_grab.Why());
        // `touch` runs inside playerGrabbed, so the lane's watch and its report went out before this line.
        Say("c-found A");
        CGo(CS::FindWaitHost);
        return;
    }
    case CS::FindWaitHost: {
        if (!Seen("h-found C")) { Late("the host never said it found C"); return; }
        void* const c = g_t[1].Get();
        if (!c) return Invalid("this client's copy of C is gone");
        Fact("find (b): this client's copy of C reads isUsed=%d music=%d", D::ReadFound(c), D::ReadMusicOn(c));
        Say("c-read C " + std::to_string(D::ReadFound(c)) + " " + std::to_string(D::ReadMusicOn(c)));
        Finish();
        return;
    }

    // ---- pocket ----
    case CS::PocketGrabB: {
        const GrabRun::R r = g_grab.Tick();
        if (r == GrabRun::R::Working) { Late("the aimed grab never ended"); return; }
        if (r == GrabRun::R::Failed) return Invalid(g_grab.Why());
        // The let-go sends the host a release, whose drive parks this copy (physics off) until the host's End: a `Hold Object` the
        // host grants meanwhile replays on a parked copy, which the verb does not take, and the commit aborts. So the verb waits
        // for that drive's End, the edge the stream records as a closed generation.
        g_letGoClosed = PDS::dev::ClosedGen(static_cast<uint32_t>(g_eid[0]), g_letGoGen);
        CGo(CS::PocketWaitDriveEnd);
        return;
    }
    case CS::PocketWaitDriveEnd: {
        uint8_t gen = 0;
        if (!PDS::dev::ClosedGen(static_cast<uint32_t>(g_eid[0]), gen) || (g_letGoClosed && gen == g_letGoGen)) {
            Late("the drive of this client's release of B never ended here");
            return;
        }
        Fact("the drive of the release of B ended here: generation %u closed (it was %s %u at the let-go)", static_cast<unsigned>(gen),
             g_letGoClosed ? "closed at" : "open,", static_cast<unsigned>(g_letGoGen));
        g_pocket.Begin(me, g_t[0].Get(), true);
        CGo(CS::PocketTake);
        return;
    }
    case CS::PocketTake: {
        const PocketRun::R r = g_pocket.Tick();
        if (r == PocketRun::R::Working) { Late("the pocket and drop never ended"); return; }
        if (r == PocketRun::R::Failed) return Invalid(g_pocket.Why());
        Say("c-dropped");
        CGo(CS::PocketWaitHostB);
        return;
    }
    case CS::PocketWaitHostB: {
        // The host holds the copy it born from this client's drop and drops it; this client's set holds B's key only from the
        // host's word, so its mirror of that second drop is found at its bind only when the word arrived.
        std::string rest;
        if (!Rest("b-dropped ", &rest)) { Late("the host never said its drop of B's copy"); return; }
        g_dropEid = std::atoi(rest.c_str());
        if (g_dropEid == 0) return Invalid("the host's drop line of B could not be read");
        CGo(CS::PocketWaitBoundB);
        return;
    }
    case CS::PocketWaitBoundB: {
        const Rec* r = FindBound(g_key[0], g_eid[0], g_dropEid);
        if (!r) { Late("this client never bound the host's drop-born copy of B"); return; }
        SayBound("B", *r);
        CGo(CS::PocketWaitHostD);
        return;
    }
    case CS::PocketWaitHostD: {
        std::string rest;
        if (!Rest("h-dropped ", &rest)) { Late("the host never said its drop of D"); return; }
        g_dropEid = std::atoi(rest.c_str());
        if (g_dropEid == 0) return Invalid("the host's drop line could not be read");
        CGo(CS::PocketWaitBoundD);
        return;
    }
    case CS::PocketWaitBoundD: {
        const Rec* r = FindBound(g_key[1], g_eid[1], g_dropEid);
        if (!r) { Late("this client never bound the host's drop-born copy of D"); return; }
        SayBound("D", *r);
        Finish();
        return;
    }

    // ---- loaded ----
    case CS::LoadedTake: {
        const PocketRun::R r = g_pocket.Tick();
        if (r == PocketRun::R::Working) { Late("the pocket and drop never ended"); return; }
        if (r == PocketRun::R::Failed) return Invalid(g_pocket.Why());
        Say("c-dropped");
        Finish();
        return;
    }

    // ---- join ----
    case CS::JoinWaitDropped: {
        std::string rest;
        if (!Rest("h-dropped ", &rest)) return;   // the host's own wait has the run's budget for a bound
        g_dropEid = std::atoi(rest.c_str());
        if (g_dropEid == 0) return Invalid("the host's drop line could not be read");
        CGo(CS::JoinWaitBound);
        return;
    }
    case CS::JoinWaitBound: {
        // The joiner knows no key: the host's drop-born copy is named by its element id.
        const Rec* r = FindBound(std::wstring(), 0, g_dropEid);
        if (!r) { Late("this client never bound the host's drop-born copy of F"); return; }
        SayBound("F", *r);
        Finish();
        return;
    }
    }
}

}  // namespace

void ClientTick() { ClientStep(); }

void ResetClient() {
    g_c = CS::WaitTargets;
    g_t[0].Reset();
    g_t[1].Reset();
    g_eid[0] = g_eid[1] = 0;
    g_key[0].clear();
    g_key[1].clear();
    g_dropEid = 0;
    g_letGoClosed = false;
    g_letGoGen = 0;
}

}  // namespace coop::dev::dingus_drill::detail
