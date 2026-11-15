// coop/world/basalt_pillar.cpp -- see coop/world/basalt_pillar.h.

#include "coop/world/basalt_pillar.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/world/world_actor_sync.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::basalt_pillar {
namespace {

namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

constexpr const wchar_t* kPillarClass = L"hexpillar_C";

UE_FACT_FN(kRowDigUp, "basalt_pillar", L"prop_hexpillar_C", L"digUp", Declared);
UE_FACT_FN(kRowUpd, "basalt_pillar", L"hexpillar_C", L"upd", Declared);
constexpr int kTagDig = 0x50444947;  // 'PDIG'

constexpr long long kPollMs = 2000;
constexpr float kStep = 0.005f;   // an `a` this far from the one sent is sent: 10 cm of the pillar's 20 m

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;
Clock::time_point g_lastPoll{};

// HOST: the `a` last sent for each pillar, by the actor's internal index.
struct SentA {
    void* actor;
    float a;
};
std::unordered_map<int32_t, SentA> g_sentA;
bool g_resendAll = false;

// CLIENT: an `a` whose copy was not there yet, by element id.
std::unordered_map<uint32_t, float> g_waiting;

float* GrowthOf(void* pillar) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(pillar), L"a");
    return off < 0 ? nullptr : reinterpret_cast<float*>(static_cast<uint8_t*>(pillar) + off);
}

// A client's dig goes to the host and does not run here. A prop with neither key nor element id is no shared prop and is
// dug here, as in single player.
sg::Verdict OnDigPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!c.object || !R::IsLive(c.object)) return sg::Verdict::Run;
    coop::net::PropIgniteIntentPayload p{};
    const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (!keyW.empty() && keyW != L"None") coop::net::WireKeyFromString(keyW, p.key);
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(c.object);
    p.elementId = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
    p.verb = kVerbDig;
    if (p.key.len == 0 && p.elementId == 0) return sg::Verdict::Run;
    const bool sent = s->SendReliable(coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p));
    UE_LOGI("basalt_pillar: CLIENT dug the buried basalt key='%ls' eid=%u -- the host is asked to: %d", keyW.c_str(),
            p.elementId, sent ? 1 : 0);
    return sg::Verdict::Cancel;
}

struct Collect {
    std::vector<void*> found;
};

void HostPoll(coop::net::Session* s) {
    void* cls = ue_wrap::object_index::ClassByName(kPillarClass);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!cls || !world) return;   // no pillar was ever minted in this process
    Collect c;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<Collect*>(ctx)->found.push_back(obj);
    }, &c);
    const bool connected = s->connected();
    int sent = 0;
    for (void* pillar : c.found) {
        if (!R::IsLive(pillar) || R::NameStartsWith(R::NameOf(pillar), L"Default__")) continue;
        if (ue_wrap::world_identity::WorldOf(pillar) != world) continue;
        const float* a = GrowthOf(pillar);
        if (!a) continue;
        const int32_t idx = R::InternalIndexOf(pillar);
        auto it = g_sentA.find(idx);
        const bool fresh = it == g_sentA.end() || it->second.actor != pillar;
        if (!fresh && !g_resendAll && std::fabs(*a - it->second.a) < kStep) continue;
        if (!connected) continue;
        // The lane's own number for it: a pillar the save loaded is enrolled here, one the lane holds answers with its row.
        const unsigned eid = coop::world_actor_sync::HostEnrollExSpawn(pillar);
        if (eid == 0) continue;
        Payload p{};
        p.id = eid;
        p.x = *a;
        p.family = coop::net::ambient_fixture::kPillarGrowth;
        p.alive = 1;
        if (!s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p))) continue;
        g_sentA[idx] = {pillar, *a};
        ++sent;
    }
    if (connected) g_resendAll = false;
    for (auto it = g_sentA.begin(); it != g_sentA.end();)
        it = R::IsLiveByIndex(it->second.actor, it->first) ? std::next(it) : g_sentA.erase(it);
    if (sent > 0) UE_LOGI("basalt_pillar: HOST sent the height of %d pillar(s) (%zu stand)", sent, g_sentA.size());
}

// The copy shows `a` through the game's own upd(). False while the copy is not there.
bool ShowGrowth(uint32_t eid, float a) {
    coop::element::WorldActor* el = coop::element::WaMirrors().Get(static_cast<coop::element::ElementId>(eid));
    void* pillar = el ? el->GetActor() : nullptr;
    if (!pillar || !R::IsLiveByIndex(pillar, el->GetInternalIdx())) return false;
    float* mine = GrowthOf(pillar);
    const auto upd = ue_wrap::facts::Resolve(kRowUpd);
    if (!mine || !upd.Fn()) {
        UE_LOGE("basalt_pillar: a pillar's `a` or its upd() did not resolve -- the copy stays as it was born");
        return true;
    }
    *mine = a;
    R::CallFunction(pillar, upd.Fn(), nullptr);
    return true;
}

void ClientPoll() {
    for (auto it = g_waiting.begin(); it != g_waiting.end();)
        it = ShowGrowth(it->first, it->second) ? g_waiting.erase(it) : std::next(it);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    if (!sg::WatchName(kRowDigUp, kTagDig, &OnDigPre, nullptr))
        UE_LOGE("basalt_pillar: the watch on prop_hexpillar_C::digUp did not register -- a client's dig destroys its own "
                "copy of the buried basalt and raises no pillar");
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && g_waiting.empty()) return;
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < kPollMs) return;
    g_lastPoll = now;
    if (host) HostPoll(s);
    else ClientPoll();
}

bool TakeDigIntent(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::PropIgniteIntentPayload p{};
    if (!bytes || len < sizeof(p)) return false;
    std::memcpy(&p, bytes, sizeof(p));
    if (p.verb != kVerbDig) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || p.key.len > sizeof(p.key.data)) return true;
    // A key resolves by the index and never falls back to the eid, which may name another prop by now.
    void* prop = p.key.len > 0 ? coop::prop_key_index::Find(coop::net::StringFromWireKey(p.key))
                               : coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
    if (!prop || !R::IsLive(prop) || _wcsicmp(R::ClassNameOf(prop).c_str(), L"prop_hexpillar_C") != 0) {
        UE_LOGW("basalt_pillar: slot %d asked to dig eid=%u, which is no buried basalt here -- dropped", senderSlot,
                p.elementId);
        return true;
    }
    const auto dig = ue_wrap::facts::Resolve(kRowDigUp);
    const bool ran = dig.Fn() && R::CallFunction(prop, dig.Fn(), nullptr);
    UE_LOGI("basalt_pillar: HOST ran slot %d's dig of the buried basalt: %d", senderSlot, ran ? 1 : 0);
    return true;
}

void OnGrowth(const Payload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(p.id) || p.x < -1.f || p.x > 2.f) {
        UE_LOGW("basalt_pillar: a height %.3f for element %u -- dropped", p.x, p.id);
        return;
    }
    if (!ShowGrowth(p.id, p.x)) g_waiting[p.id] = p.x;   // its spawn is on the way
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_resendAll = true; }

void OnDisconnect() {
    g_waiting.clear();
    g_sentA.clear();
    g_resendAll = false;
}

}  // namespace coop::basalt_pillar
