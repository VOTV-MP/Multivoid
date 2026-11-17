// coop/dev/pile_orphan_drill.cpp -- see coop/dev/pile_orphan_drill.h.

#include "coop/dev/pile_orphan_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/props/pile_spawn_bind.h"
#include "coop/props/save_time_retire_util.h"  // kExactMatchR2Cm: the 1 cm the bind matches by
#include "coop/session/rig_verdict.h"
#include "harness/autotest/peer_signal.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace coop::dev::pile_orphan_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace PS = harness::autotest::peer_signal;
namespace V = coop::rig_verdict;

constexpr const char* kTag = "pile-orphan";
constexpr const char* kSignal = "pile-orphan-staged";
constexpr float kIsolateCm = 10.f;   // no other trash native this close to a staged one, so a 1 cm look finds only it

struct Spot {
    std::wstring cls;
    ue_wrap::FVector at{};
};

bool g_tried = false;        // peer_signal::Begin ran
bool g_hostStaged = false;   // once per process: the host stages for the first capture
Spot g_pile, g_clump;        // the client's reading of the host's signal
bool g_preOk = false;        // the client held a native at both spots when the sweep began
bool g_done = false;

bool Armed() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::pile_orphan_drill);
    return on;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
    return s;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// `<class> x y z;<class> x y z`, the pile then the clump.
std::string Encode(const Spot& pile, const Spot& clump) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s %.4f %.4f %.4f;%s %.4f %.4f %.4f", Narrow(pile.cls).c_str(), pile.at.X,
                  pile.at.Y, pile.at.Z, Narrow(clump.cls).c_str(), clump.at.X, clump.at.Y, clump.at.Z);
    return buf;
}

bool Decode(std::string text, Spot& pile, Spot& clump) {
    for (char& c : text) if (c == ';') c = ' ';
    std::istringstream in(text);
    std::string pc, kc;
    if (!(in >> pc >> pile.at.X >> pile.at.Y >> pile.at.Z >> kc >> clump.at.X >> clump.at.Y >> clump.at.Z)) return false;
    pile.cls = Widen(pc);
    clump.cls = Widen(kc);
    return true;
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

// A live native of `s`'s class within 1 cm of its spot, or null.
void* FindAt(const std::vector<void*>& natives, const Spot& s) {
    for (void* a : natives) {
        if (R::ClassNameOf(a) != s.cls) continue;
        ue_wrap::FVector p{};
        if (!ue_wrap::engine::TryGetActorLocation(a, p)) continue;
        if (Dist2(p, s.at) <= coop::save_time_retire_util::kExactMatchR2Cm) return a;
    }
    return nullptr;
}

struct Cand { void* a; ue_wrap::FVector p; };

// The first live trash native of the wanted form with no other trash native within kIsolateCm, or null.
const Cand* PickIsolated(const std::vector<Cand>& cs, bool clump) {
    for (const Cand& c : cs) {
        if (clump ? !ue_wrap::prop::IsGarbageClump(c.a) : !ue_wrap::prop::IsChipPile(c.a)) continue;
        bool alone = true;
        for (const Cand& d : cs) {
            if (d.a != c.a && Dist2(c.p, d.p) < kIsolateCm * kIsolateCm) { alone = false; break; }
        }
        if (alone) return &c;
    }
    return nullptr;
}

}  // namespace

void Init() {
    if (!Armed() || g_tried) return;
    g_tried = true;
    if (!PS::Begin(kTag)) { g_done = true; return; }   // Begin said INVALID itself
    UE_LOGI("pile-orphan: signal clock started, dir='%s'", PS::Dir().c_str());
}

void OnDisconnect() {
    g_preOk = false;
    g_done = false;
    g_pile = Spot{};
    g_clump = Spot{};
}

void NoteCaptured(int peerSlot) {
    if (!Armed() || g_hostStaged || !g_tried) return;
    g_hostStaged = true;
    std::vector<void*> natives;
    coop::pile_spawn_bind::CollectLiveTrashNatives(natives);
    std::vector<Cand> cs;
    cs.reserve(natives.size());
    for (void* a : natives) {
        Cand c{a, {}};
        if (ue_wrap::engine::TryGetActorLocation(a, c.p)) cs.push_back(c);
    }
    const Cand* pile = PickIsolated(cs, /*clump=*/false);
    const Cand* clump = PickIsolated(cs, /*clump=*/true);
    if (!pile || !clump) {
        V::Say(kTag, V::Outcome::Invalid, "the host could not stage both: %s",
               !pile ? "no isolated chip pile in its world" : "no isolated garbage clump in its world");
        g_done = true;
        return;
    }
    const Spot sp{R::ClassNameOf(pile->a), pile->p};
    const Spot sc{R::ClassNameOf(clump->a), clump->p};
    ue_wrap::engine::DestroyActor(pile->a);
    ue_wrap::engine::DestroyActor(clump->a);
    const std::string text = Encode(sp, sc);
    PS::Mark(kSignal, text);
    UE_LOGI("pile-orphan: STAGED slot=%d pile='%ls' (%.1f,%.1f,%.1f) clump='%ls' (%.1f,%.1f,%.1f) -- destroyed on the "
            "host after the capture of slot %d's world; the client's natives there are orphans", peerSlot,
            sp.cls.c_str(), sp.at.X, sp.at.Y, sp.at.Z, sc.cls.c_str(), sc.at.X, sc.at.Y, sc.at.Z, peerSlot);
}

void NoteNoSweep() {
    if (!Armed() || g_done) return;
    g_done = true;
    V::Say(kTag, V::Outcome::Invalid, "the join sweep never ran: no snapshot bracket arrived, so there is nothing to judge");
}

void NoteSweepBegin() {
    if (!Armed() || g_done) return;
    std::string text;
    const bool peeked = g_tried && PS::Peek(kSignal, text);
    const bool decoded = peeked && Decode(text, g_pile, g_clump);
    UE_LOGI("pile-orphan: at sweep begin tried=%d peek=%d decode=%d text='%s'", g_tried ? 1 : 0, peeked ? 1 : 0,
            decoded ? 1 : 0, text.c_str());
    if (!decoded) {
        V::Say(kTag, V::Outcome::Invalid, "no staging signal from the host when the sweep began");
        g_done = true;
        return;
    }
    std::vector<void*> natives;
    coop::pile_spawn_bind::CollectLiveTrashNatives(natives);
    if (!FindAt(natives, g_pile) || !FindAt(natives, g_clump)) {
        V::Say(kTag, V::Outcome::Invalid,
               "the client holds no native '%ls' at the staged pile spot or no '%ls' at the clump spot: its world "
               "was cut after the host lost them, so there is no orphan to judge",
               g_pile.cls.c_str(), g_clump.cls.c_str());
        g_done = true;
        return;
    }
    g_preOk = true;
    UE_LOGI("pile-orphan: client holds both orphans before the sweep ('%ls' and '%ls')", g_pile.cls.c_str(),
            g_clump.cls.c_str());
}

void NoteSweepDone(bool aborted) {
    if (!Armed() || g_done || !g_preOk) return;
    g_done = true;
    std::vector<void*> natives;
    coop::pile_spawn_bind::CollectLiveTrashNatives(natives);
    const bool pileAlive = FindAt(natives, g_pile) != nullptr;
    const bool clumpAlive = FindAt(natives, g_clump) != nullptr;
    if (pileAlive || clumpAlive) {
        V::Say(kTag, V::Outcome::Fail, "%s%s%s survived the join sweep%s", pileAlive ? "the orphan chip pile" : "",
               (pileAlive && clumpAlive) ? " and " : "", clumpAlive ? "the orphan clump" : "",
               aborted ? " (the sweep was aborted by its >50% valve)" : "");
        return;
    }
    V::Say(kTag, V::Outcome::Pass, "the orphan chip pile '%ls' and clump '%ls' were both removed by the join sweep",
           g_pile.cls.c_str(), g_clump.cls.c_str());
}

}  // namespace coop::dev::pile_orphan_drill
