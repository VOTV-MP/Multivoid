// coop/creatures/boar_sync.cpp -- see coop/creatures/boar_sync.h.

#include "coop/creatures/boar_sync.h"

#include "coop/element/element.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace coop::boar_sync {
namespace {

namespace R = ue_wrap::reflection;

constexpr uint64_t kPollMs = 500;
constexpr int32_t  kMaxBoars = 64;  // the war holds seven and the night spawner five; past this the list is not read

uint64_t g_nextMs = 0;
int32_t  g_listOff = -1;      // mainGamemode_C::allGrayboars
bool     g_listFailed = false;
int32_t  g_saidBoars = -1;    // the counts last logged, so a line is written when they change
int32_t  g_saidOpen = -1;

struct ArrayHead {
    void* const* data;
    int32_t      num;
};

}  // namespace

void Tick(coop::net::Session* s) {
    if (!s || !s->connected() || s->role() != coop::net::Role::Host || s->connectedPeerCount() <= 0) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kPollMs;
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm || g_listFailed) return;
    if (g_listOff < 0) {
        g_listOff = R::FindPropertyOffset(R::ClassOf(gm), L"allGrayboars");
        if (g_listOff < 0) {
            g_listFailed = true;
            UE_LOGW("boar_sync: mainGamemode_C.allGrayboars did not resolve -- a boar moves on the host only");
            return;
        }
    }
    const auto& arr = *reinterpret_cast<const ArrayHead*>(static_cast<const uint8_t*>(gm) + g_listOff);
    if (!arr.data || arr.num <= 0 || arr.num > kMaxBoars) {
        if (g_saidBoars > 0) UE_LOGI("boar_sync: no boar is listed any more");
        g_saidBoars = 0;
        g_saidOpen = 0;
        return;
    }
    int32_t boars = 0, open = 0;
    for (int32_t i = 0; i < arr.num; ++i) {
        void* boar = arr.data[i];
        if (!boar || !R::IsLive(boar)) continue;
        ++boars;
        const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(boar);
        if (eid == coop::element::kInvalidId || eid == 0u) continue;  // not a prop the host has named (yet)
        coop::prop_drive_host::Claim(boar, "boar");
        if (coop::prop_drive_host::ContextOf(boar, static_cast<uint32_t>(eid)).open) ++open;
    }
    if (boars != g_saidBoars || open != g_saidOpen) {
        g_saidBoars = boars;
        g_saidOpen = open;
        UE_LOGI("boar_sync: %d boar(s) listed, %d streamed to the clients", boars, open);
    }
}

void OnDisconnect() {
    g_nextMs = 0;
    g_saidBoars = -1;
    g_saidOpen = -1;
}

}  // namespace coop::boar_sync
