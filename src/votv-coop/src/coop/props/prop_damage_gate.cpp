// coop/props/prop_damage_gate.cpp -- see coop/props/prop_damage_gate.h.

#include "coop/props/prop_damage_gate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::props::prop_damage_gate {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// comp_physicsImpact_C's own two damage entries: fireDamage ends in the owner's destroy without passing impactDamage.
UE_FACT_FN(kRowImpact, "prop_damage_gate", L"comp_physicsImpact_C", L"impactDamage", Declared);
UE_FACT_FN(kRowFire, "prop_damage_gate", L"comp_physicsImpact_C", L"fireDamage", Declared);

enum Row { kImpact, kFire };
constexpr int kRows = 2;
constexpr int kTag[kRows] = {0x50444731, 0x50444732};  // 'PDG1', 'PDG2'
const ue_wrap::facts::Function* const kRowOf[kRows] = {&kRowImpact, &kRowFire};
const char* const kVerb[kRows] = {"impactDamage", "fireDamage"};

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// Never set in a shipped run: the gate does nothing (the red of the drill's break and fire arms).
bool Off() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::prop_damage_gate_off);
    return s;
}

// Never set in a shipped run: a client's own authorship no longer spares a prop (the red of the held-break arm).
bool Strict() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::prop_damage_gate_strict);
    return s;
}

// This machine authors the prop's motion: it holds it in a hand grab, it threw it, or its prop-syncer episode names it.
bool AuthoredHere(void* prop) {
    void* me = coop::players::Registry::Get().Local();
    if (me && ue_wrap::engine::IsMainPlayerGrabbing(me, prop)) return true;
    if (coop::prop_throw::SeqFor(prop) != 0) return true;
    return coop::prop_syncer::AuthorsActorHere(prop);
}

unsigned long long                g_cancelled[kRows] = {};
ue_wrap::ObjKeyedCache<bool, 24>  g_said[kRows];   // the owner classes whose first cancelled call has been said

sg::Verdict Judge(Row row, const sg::Call& c) {
    if (Off()) return sg::Verdict::Run;
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    void* owner = R::OuterOf(c.object);
    if (!owner || !ue_wrap::prop::IsDescendantOfProp(owner)) return sg::Verdict::Run;
    if (!Strict() && AuthoredHere(owner)) return sg::Verdict::Run;

    // MTA lets the mirror break locally and sends nothing:
    // reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4894-4925. The server's is the shared break:
    // reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:8489-8505.
    // Here the call is cancelled instead, since a mirror that breaks alone stands apart from the host's prop until the
    // host's destroy arrives.
    ++g_cancelled[row];
    void* const cls = R::ClassOf(owner);
    if (!g_said[row].Find(cls)) {
        g_said[row].Insert(cls, true);
        UE_LOGI("prop_damage_gate: a client's %s on a prop it does not author is the host's (cls=%ls)", kVerb[row],
                R::ClassNameOf(owner).c_str());
    }
    return sg::Verdict::Cancel;
}

sg::Verdict OnImpactPre(const sg::Call& c) { return Judge(kImpact, c); }
sg::Verdict OnFirePre(const sg::Call& c) { return Judge(kFire, c); }

const sg::PreFn kPre[kRows] = {&OnImpactPre, &OnFirePre};

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRows] = {Reg::Pending, Reg::Pending};
bool g_settled[kRows] = {};

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session || session->role() != coop::net::Role::Client) return;
    // A client cancels the call by these watches; the gate refuses a watch it cannot keep for the process.
    for (int i = 0; i < kRows; ++i)
        if (g_reg[i] == Reg::Pending)
            g_reg[i] = sg::WatchName(*kRowOf[i], kTag[i], kPre[i], nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    bool resolved = false;
    for (int i = 0; i < kRows; ++i) {
        if (g_settled[i] || g_reg[i] == Reg::Pending) continue;
        if (g_reg[i] == Reg::Registered && !resolved) {
            sg::ResolvePendingNames();
            resolved = true;
        }
        if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRowOf[i], kTag[i])) {
            g_settled[i] = true;
            UE_LOGI("prop_damage_gate: the comp_physicsImpact_C %s watch is live", kVerb[i]);
        } else if (g_reg[i] == Reg::Refused || sg::NameWatchSettled(*kRowOf[i], kTag[i])) {
            g_settled[i] = true;
            UE_LOGE("prop_damage_gate: the %s watch is dead -- a client's %s on a prop it does not author runs",
                    kVerb[i], kVerb[i]);
        }
    }
}

void OnDisconnect() {
    if (g_cancelled[kImpact] != 0 || g_cancelled[kFire] != 0)
        UE_LOGI("prop_damage_gate: %llu impactDamage and %llu fireDamage calls cancelled this session",
                g_cancelled[kImpact], g_cancelled[kFire]);
    for (int i = 0; i < kRows; ++i) {
        g_cancelled[i] = 0;
        g_said[i].Clear();
    }
}

}  // namespace coop::props::prop_damage_gate
