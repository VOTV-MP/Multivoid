// coop/creatures/wolfgang_sync.cpp -- see coop/creatures/wolfgang_sync.h.
//
// 0.9.0n. antibreatherSPawner_C's begin play loops on a 10 s delay: with a pumpkin in its sphere it runs `cencipede`,
// which mints wolfgangEggs_C (a prop) when a nest (eggyWolfgang_C) stands, else a centipedeHead_C at the spawner.
// centipedeHead_C's begin play mints its `segm` class, segment by segment; its tick moves them. wolfgangJr_C and
// parentWolfgang_C have no graph of their own (centipedeHead_C children); eggyWolfgang_C's tick mints a
// parentWolfgang_C.

#include "coop/creatures/wolfgang_sync.h"

#include "coop/net/session.h"
#include "coop/world/ambient_births.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace coop::wolfgang_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;
namespace WA = coop::world_actor_sync;

UE_FACT_FN(kRowCencipede, "wolfgang_sync", L"antibreatherSPawner_C", L"cencipede", Declared);

constexpr int kTag = 0x574F4C46;   // 'WOLF'

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
std::vector<coop::ambient_births::Event> g_births;
int g_ended = 0;

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Client;
}

// CLIENT: Wolfgang comes out of the host's cave alone.
sg::Verdict OnCencipedePre(const sg::Call& call) {
    if (call.tag != kTag || !IsClient()) return sg::Verdict::Run;
    static int s_refused = 0;
    if (++s_refused == 1 || (s_refused & (s_refused - 1)) == 0)
        UE_LOGI("wolfgang: this client's cave spawner would mint Wolfgang -- refused, the host's crosses (%d)", s_refused);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_watched && (g_watched = sg::WatchName(kRowCencipede, kTag, &OnCencipedePre, nullptr)))
        UE_LOGI("wolfgang: the cave spawner's cencipede is watched");
    // Every part's birth (the save's at the hub's seed): the host enrols it, a client ends its own at once.
    const bool host = s->role() == N::Role::Host;
    if (!host && WA::IsMaterializingMirror()) return;
    g_births.clear();
    coop::ambient_births::Drain(coop::ambient_births::kWolfgang, g_births);
    for (const auto& e : g_births) {
        void* const a = e.obj;
        if (e.kind != coop::ambient_births::Kind::Birth || !R::IsLiveByIndex(a, e.index)) continue;
        if (host) {
            WA::HostEnrollExSpawn(a);   // the save's and any the catch missed; the enrolled answer at once
        } else if (!WA::IsMirroredActor(a) && E::DestroyActor(a) && (++g_ended == 1 || (g_ended & (g_ended - 1)) == 0)) {
            UE_LOGI("wolfgang: this client's own %ls ended at its birth -- the host's crosses (%d so far)",
                    R::ClassNameOf(a).c_str(), g_ended);
        }
    }
}

void OnDisconnect() {
    g_births.clear();
    g_ended = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::wolfgang_sync
