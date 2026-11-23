// coop/creatures/nisse_sync.cpp -- see coop/creatures/nisse_sync.h.
//
// 0.9.0n. prop_xmastree_C::ReceiveTick marks `sleptThroughMidnight` while its player sleeps past midnight.
// prop_xmastree_C::woken (bedEvent_C's wake, cig_C) returns unless that mark is set, clears it, checks the gamemode's
// time, returns if a nisse_soul_C stands, then mints three tomtenisse_C beside the tree (one with `gift`, one `blu`)
// and runs fillBoots.

#include "coop/creatures/nisse_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/anomaly_ask.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <atomic>
#include <cstdint>

namespace coop::nisse_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowWoken, "nisse_sync", L"prop_xmastree_C", L"woken", Declared);

constexpr const wchar_t* kTreeClass = L"prop_xmastree_C";
constexpr int kTagWoken = 0x4E495357;   // 'NISW'

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Client;
}

// The tree's night mark: its byte and mask, or false.
bool Mark(void* tree, uint8_t*& byte, uint8_t& mask) {
    int32_t off = -1;
    if (!tree || !R::FindBoolProperty(R::ClassOf(tree), L"sleptThroughMidnight", off, mask)) return false;
    byte = static_cast<uint8_t*>(tree) + off;
    return true;
}

// CLIENT: the tree's wake is the host's; a marked night is asked of it.
sg::Verdict OnWokenPre(const sg::Call& call) {
    if (call.tag != kTagWoken || !IsClient()) return sg::Verdict::Run;
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (Mark(call.object, byte, mask) && (*byte & mask)) {
        *byte = static_cast<uint8_t>(*byte & ~mask);
        const bool sent = coop::anomaly_ask::Send(N::kAnomalyAskNisseWoken, 0.f, 0.f, 0.f);
        UE_LOGI("nisse: this client's player woke through midnight with the tree up -- asked of the host (sent=%d)",
                sent ? 1 : 0);
    }
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || g_watched) return;
    if ((g_watched = sg::WatchName(kRowWoken, kTagWoken, &OnWokenPre, nullptr)))
        UE_LOGI("nisse: the Christmas tree's wake is watched");
}

void OnWokenAsk(uint8_t slot) {
    void* tree[1];
    if (ue_wrap::world_instances::Find(kTreeClass, tree, 1) < 1 || !tree[0] || !R::IsLive(tree[0])) {
        UE_LOGW("nisse: slot %u woke through midnight -- refused: no Christmas tree stands here", slot);
        return;
    }
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (!Mark(tree[0], byte, mask)) return;
    *byte = static_cast<uint8_t>(*byte | mask);
    ue_wrap::ParamFrame pf(R::FindDispatchFunction(R::ClassOf(tree[0]), L"woken", nullptr));
    const bool ran = pf.valid() && ue_wrap::Call(tree[0], pf);
    UE_LOGI("nisse: slot %u woke through midnight -- the host's tree woke (ran=%d)", slot, ran ? 1 : 0);
}

void OnDisconnect() { g_session.store(nullptr, std::memory_order_release); }

}  // namespace coop::nisse_sync
