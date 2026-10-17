// coop/dev/explosion_drill_client.cpp -- see coop/dev/explosion_drill_internal.h.

#include "coop/dev/explosion_drill_internal.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/explosion_parking.h"
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::dev::explosion_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace JP = coop::join_progress;
namespace ES = coop::explosion_sync;

namespace {

constexpr auto  kClientBound = std::chrono::seconds(120);  // from the client's world-ready edge

enum class CPhase : uint8_t { Edge, Join, Rest, Fixture, Apply, Malformed, Find, Settle, Blast, Fly, Done };

struct Client {
    CPhase            phase = CPhase::Edge;
    Clock::time_point edgeAt{}, at{}, foundAt{};
    ES::Counters      sync{};
    coop::explosion_parking::Counters park{};
    Rest              pawn;
    Vec               B{};
    ue_wrap::CachedObjRef copy[kPapers];
    uint32_t          eid[kPapers] = {};
    Rest              rest[kPapers];
    Vec               base[kPapers];
    bool              moved[kPapers] = {};
    std::vector<uint32_t> prePapers;  // the paper elements already here at the world-ready edge
};
Client g_clientRun;
std::vector<coop::element::Registry::ActorIdPair> g_snap;  // reused by every find

void ClientEnter(CPhase p) {
    g_clientRun.phase = p;
    g_clientRun.at = Clock::now();
}
void ClientAbandon(const char* what) {
    Warn("ABANDONED %s", what);
    g_clientRun.phase = CPhase::Done;
}
void ClientFail(const char* arm, const char* what) {
    Warn("FAIL %s %s", arm, what);
    g_clientRun.phase = CPhase::Done;
}
bool EdgeLate() { return Past(g_clientRun.edgeAt, kClientBound); }

// Apply: each class applied once, its force narrowed and its rune check refused, nothing dropped. PASS as soon as
// every reading holds; else, at the bound, the first reading that does not.
void ClientApply() {
    Client& c = g_clientRun;
    const ES::Counters sy = ES::GetCounters();
    const coop::explosion_parking::Counters pk = coop::explosion_parking::GetCounters();
    uint32_t dropped = 0;
    for (int i = 0; i < static_cast<int>(ES::DropReason::Count); ++i) dropped += sy.dropped[i] - c.sync.dropped[i];
    const bool applied = sy.applied[0] - c.sync.applied[0] == 1 && sy.applied[1] - c.sync.applied[1] == 1 &&
                         sy.applied[2] - c.sync.applied[2] == 1;
    const bool narrowed = pk.narrowed - c.park.narrowed == 3;
    const bool rune = pk.runeRefused - c.park.runeRefused == 3;
    const bool dropClean = dropped == 0;
    const bool narrowClean = pk.narrowFailed == c.park.narrowFailed;
    if (applied && narrowed && rune && dropClean && narrowClean) {
        uint32_t calls[4], refused[4];
        for (int i = 0; i < 4; ++i) {
            calls[i] = pk.verbCalls[i] - c.park.verbCalls[i];
            refused[i] = pk.verbRefused[i] - c.park.verbRefused[i];
        }
        Info("client DONE PASS arm=apply verbs calls=%u/%u/%u/%u refused=%u/%u/%u/%u", calls[0], calls[1], calls[2],
             calls[3], refused[0], refused[1], refused[2], refused[3]);
        c.phase = CPhase::Done;
        return;
    }
    if (!EdgeLate()) return;
    ClientFail("apply", !applied ? "applied" : !narrowed ? "narrowed" : !rune ? "runeRefused"
                      : !dropClean ? "dropped" : "narrowFailed");
}

// Spawn_malformed: four drops, one each by its reason, and the one valid message applied.
void ClientMalformed() {
    Client& c = g_clientRun;
    const ES::Counters sy = ES::GetCounters();
    auto d = [&](ES::DropReason r) {
        const int i = static_cast<int>(r);
        return sy.dropped[i] - c.sync.dropped[i];
    };
    const uint32_t nonfinite = d(ES::DropReason::NonFinite), cls = d(ES::DropReason::Class),
                   range = d(ES::DropReason::Range), flags = d(ES::DropReason::Flags),
                   applied = sy.applied[0] - c.sync.applied[0];
    if (nonfinite == 1 && cls == 1 && range == 1 && flags == 1 && applied == 1) {
        Info("client DONE PASS arm=spawn_malformed");
        c.phase = CPhase::Done;
        return;
    }
    if (!EdgeLate()) return;
    ClientFail("spawn_malformed", nonfinite != 1 ? "nonfinite" : cls != 1 ? "class" : range != 1 ? "range"
                                : flags != 1 ? "flags" : "applied");
}

// The prop_paper_C elements this client has, by element id; the class test before any reflected read.
void PaperIds(std::vector<uint32_t>& out) {
    out.clear();
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    for (const auto& p : g_snap)
        if (p.actor && R::IsLiveByIndex(p.actor, p.internalIdx) &&
            R::NameEquals(R::NameOf(R::ClassOf(p.actor)), L"prop_paper_C"))
            out.push_back(p.id);
}

// The prop_paper_C elements that arrived after the world-ready edge -- the host's four: how many, and the first four.
int FindPapers(const std::vector<uint32_t>& pre, void* out[kPapers], uint32_t eid[kPapers]) {
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    int n = 0;
    for (const auto& p : g_snap) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx) ||
            !R::NameEquals(R::NameOf(R::ClassOf(p.actor)), L"prop_paper_C") ||
            std::find(pre.begin(), pre.end(), p.id) != pre.end())
            continue;
        if (n < kPapers) {
            out[n] = p.actor;
            eid[n] = p.id;
        }
        ++n;
    }
    return n;
}

// Every copy's rest, one Tick. False (abandoned) when a copy is gone.
bool TrackCopies(Client& c, Vec at[kPapers]) {
    for (int i = 0; i < kPapers; ++i) {
        void* a = c.copy[i].Get();
        if (!a || !E::TryGetActorLocation(a, at[i])) {
            char what[48];
            std::snprintf(what, sizeof(what), "a paper copy is gone eid=%u", static_cast<unsigned>(c.eid[i]));
            ClientAbandon(what);
            return false;
        }
        NoteMoved(c.rest[i], at[i]);
    }
    return true;
}
bool AllRest(const Client& c) {
    for (int i = 0; i < kPapers; ++i)
        if (!AtRest(c.rest[i])) return false;
    return true;
}

void ClientCoast() {
    Client& c = g_clientRun;
    Vec at[kPapers] = {};
    switch (c.phase) {
    case CPhase::Find: {
        if (ES::GetCounters().applied[0] != c.sync.applied[0])
            return ClientAbandon("the blast arrived before the paper copies were found");
        void* found[kPapers];
        uint32_t ids[kPapers];
        const int n = FindPapers(c.prePapers, found, ids);
        if (n > kPapers) return ClientAbandon("more than four new papers");
        if (n < kPapers) {
            if (Past(c.at, kBound)) ClientAbandon("the four paper copies were not found");
            return;
        }
        for (int i = 0; i < kPapers; ++i) {
            c.copy[i].Set(found[i]);
            c.eid[i] = ids[i];
            c.rest[i] = Rest{};
        }
        c.foundAt = Clock::now();
        c.phase = CPhase::Settle;
        return;
    }
    case CPhase::Settle:
        if (ES::GetCounters().applied[0] != c.sync.applied[0])
            return ClientAbandon("the blast arrived before the paper copies rested");
        if (!TrackCopies(c, at)) return;
        if (AllRest(c)) {
            c.phase = CPhase::Blast;
        } else if (Past(c.foundAt, kBound)) {
            ClientAbandon("the paper copies never came to rest");
        }
        return;
    case CPhase::Blast:
    case CPhase::Fly: {
        if (!TrackCopies(c, at)) return;
        if (c.phase == CPhase::Blast && ES::GetCounters().applied[0] != c.sync.applied[0]) {
            for (int i = 0; i < kPapers; ++i) c.base[i] = at[i];  // the baseline is the blast, never the papers' fall
            c.phase = CPhase::Fly;
        }
        if (c.phase == CPhase::Fly) {
            for (int i = 0; i < kPapers; ++i) {
                if (!c.moved[i] && Dist(at[i], c.base[i]) > kFlyDistance) {
                    c.moved[i] = true;
                    c.rest[i].ticks = 0;
                }
            }
            int flown = 0;
            bool rested = true;
            for (int i = 0; i < kPapers; ++i) {
                flown += c.moved[i] ? 1 : 0;
                rested = rested && AtRest(c.rest[i]);
            }
            if (flown >= kMinFlown && rested) {
                for (int i = 0; i < kPapers; ++i) RestLine(c.eid[i], at[i]);
                Info("client DONE PASS arm=coast moved=%.0f/%.0f/%.0f/%.0f", Dist(at[0], c.base[0]),
                     Dist(at[1], c.base[1]), Dist(at[2], c.base[2]), Dist(at[3], c.base[3]));
                c.phase = CPhase::Done;
                return;
            }
        }
        if (!Past(c.foundAt, kBound) && !EdgeLate()) return;
        if (c.phase == CPhase::Blast) return ClientFail("coast", "blast did not arrive");
        char what[64];
        int flown = 0;
        for (int i = 0; i < kPapers; ++i) flown += c.moved[i] ? 1 : 0;
        if (flown < kMinFlown) {
            std::snprintf(what, sizeof(what), "copies did not fly n=%d", flown);
            return ClientFail("coast", what);
        }
        for (int i = 0; i < kPapers; ++i)
            if (!AtRest(c.rest[i])) {
                std::snprintf(what, sizeof(what), "copy did not rest eid=%u", static_cast<unsigned>(c.eid[i]));
                return ClientFail("coast", what);
            }
        return;
    }
    default:
        return;
    }
}

}  // namespace

void ClientTick(Arm arm) {
    Client& c = g_clientRun;
    switch (c.phase) {
    case CPhase::Edge:
        if (!coop::net_pump::HasAnnouncedWorldReady()) return;
        c.sync = ES::GetCounters();  // before the host's fixture can blast
        c.park = coop::explosion_parking::GetCounters();
        PaperIds(c.prePapers);
        c.edgeAt = Clock::now();
        ClientEnter(CPhase::Join);
        return;
    case CPhase::Join:
        if (JP::CurrentPhase() == JP::Phase::Idle) return ClientEnter(CPhase::Rest);
        if (EdgeLate()) ClientAbandon("the join never reached idle");
        return;
    case CPhase::Rest: {
        void* pawn = coop::players::Registry::Get().Local();
        if (pawn) NoteSpeed(c.pawn, Len(E::GetActorVelocity(pawn)));
        else c.pawn = Rest{};
        if (AtRest(c.pawn)) return ClientEnter(CPhase::Fixture);
        if (Past(c.at, kBound)) ClientAbandon("the client's pawn never rested");
        return;
    }
    case CPhase::Fixture: {
        void* pawn = coop::players::Registry::Get().Local();
        const Fix f = ComputeB(pawn, pawn, c.B);
        if (f == Fix::Wait) {
            if (Past(c.at, kBound)) ClientAbandon("fixture unreadable");
            return;
        }
        if (f == Fix::Blocked) return ClientAbandon("fixture blocked");
        if (f == Fix::NoTrace) return ClientAbandon("fixture trace unresolvable");
        Info("client fixture B=(%.0f,%.0f,%.0f)", c.B.X, c.B.Y, c.B.Z);
        ClientEnter(arm == Arm::Apply ? CPhase::Apply : arm == Arm::Coast ? CPhase::Find : CPhase::Malformed);
        return;
    }
    case CPhase::Apply: return ClientApply();
    case CPhase::Malformed: return ClientMalformed();
    case CPhase::Find:
    case CPhase::Settle:
    case CPhase::Blast:
    case CPhase::Fly: return ClientCoast();
    case CPhase::Done: return;
    }
}

void ClientReset() { g_clientRun = Client{}; }

}  // namespace coop::dev::explosion_drill::detail
