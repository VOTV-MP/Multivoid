// coop/props/dingus_sync.cpp -- see coop/props/dingus_sync.h.

#include "coop/props/dingus_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/actors/prop.h"   // GetInteractableKeyString
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::dingus_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
using coop::net::DingusFoundPayload;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kLookMs = 500;
uint64_t g_nextLookMs = 0;

// The found set and, on a client, the keys it has reported, both of the running world.
std::unordered_set<std::wstring> g_found;
std::unordered_set<std::wstring> g_reported;
uint32_t g_world = 0;

void ForWorld() {
    const uint32_t now = ue_wrap::world_identity::Generation();
    if (now == g_world) return;
    g_world = now;
    g_found.clear();
    g_reported.clear();
}

// Every live dingus of every class that descends from prop_dingus_C.
std::vector<void*> Copies() {
    std::vector<void*> out;
    void* base = OI::ClassByName(L"prop_dingus_C");
    if (!base) return out;
    struct Ctx {
        std::vector<void*>* out;
        void* base;
    } ctx{&out, base};
    OI::ForEachClass([](void* c, void* cls, void*) {
        auto* x = static_cast<Ctx*>(c);
        if (!R::IsDescendantOfAny(cls, &x->base, 1)) return;
        OI::ForEachInstance(cls, [](void* c2, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<Ctx*>(c2)->out->push_back(obj);
        }, c);
    }, &ctx);
    return out;
}

bool ReadUsed(void* d, bool& used) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(d), L"isused", off, mask)) return false;
    used = (static_cast<const uint8_t*>(d)[off] & mask) != 0;
    return true;
}

// Found, as the dingus's own Touch makes it (without the finder's achievement): `isused` set and its music deactivated.
bool MarkFound(void* d) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(d), L"isused", off, mask)) return false;
    static_cast<uint8_t*>(d)[off] |= mask;
    const int32_t wsOff = R::FindPropertyOffset(R::ClassOf(d), L"ws1");
    void* const ws1 = wsOff >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(d) + wsOff) : nullptr;
    return ws1 && R::IsLive(ws1) &&
           R::CallFunction(ws1, R::FindDispatchFunctionCached(R::ClassOf(ws1), L"Deactivate"), nullptr);
}

void Send(coop::net::Session& s, const std::wstring& key, uint8_t op, int slot) {
    DingusFoundPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = op;
    if (slot >= 0) s.SendReliableToSlot(slot, coop::net::ReliableKind::DingusFound, &p, sizeof(p));
    else s.SendReliable(coop::net::ReliableKind::DingusFound, &p, sizeof(p));
}

// HOST: a key into the set, told to every client once.
void HostFound(coop::net::Session& s, const std::wstring& key, const char* how) {
    if (!g_found.insert(key).second) return;
    Send(s, key, coop::net::kDingusFoundHost, -1);
    UE_LOGI("dingus_sync: the dingus '%ls' is found (%s) -- every player's copy goes quiet", key.c_str(), how);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextLookMs) return;
    g_nextLookMs = now + kLookMs;
    ForWorld();
    const bool host = s->role() == coop::net::Role::Host;
    for (void* d : Copies()) {
        bool used = false;
        if (!ReadUsed(d, used)) continue;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(d);
        if (key.empty() || key == L"None") continue;
        if (used) {
            if (host) HostFound(*s, key, "the host's copy");
            else if (s->connected() && !g_found.count(key) && g_reported.insert(key).second)
                Send(*s, key, coop::net::kDingusFoundReport, 0);
        } else if (g_found.count(key)) {
            const bool ok = MarkFound(d);
            UE_LOGI("dingus_sync: this copy of the found dingus '%ls' marked found (music off=%d)", key.c_str(), ok ? 1 : 0);
        }
    }
}

void OnMsg(const DingusFoundPayload& p, int senderPeerSlot) {
    auto* const s = g_session.load(std::memory_order_acquire);
    if (!s || p.key.len == 0 || p.key.len > sizeof(p.key.data)) return;
    ForWorld();
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (s->role() == coop::net::Role::Host) {
        if (p.op != coop::net::kDingusFoundReport || senderPeerSlot <= 0) return;   // op 1 is the host's own
        HostFound(*s, key, "a client found it");
        return;
    }
    if (p.op != coop::net::kDingusFoundHost || senderPeerSlot != 0) return;
    g_found.insert(key);   // the next look marks this peer's copies
}

void SendJoinFound(int slot) {
    auto* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    ForWorld();
    for (const std::wstring& key : g_found) Send(*s, key, coop::net::kDingusFoundHost, slot);
}

void OnDisconnect() {
    g_found.clear();
    g_reported.clear();
}

}  // namespace coop::dingus_sync
