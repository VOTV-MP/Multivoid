// coop/props/glowstick_struck.cpp -- see coop/props/glowstick_struck.h.

#include "coop/props/glowstick_struck.h"

#include "coop/props/prop_key_index.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <unordered_map>

namespace coop::glowstick_struck {
namespace {

namespace R = ue_wrap::reflection;

struct Waiting {
    ue_wrap::CachedObjRef glowstick;
    std::wstring key;
};
std::vector<Waiting> g_waiting;
constexpr uint64_t kWalkNext = ~0ull;   // a value no key index has: the next tick walks
uint64_t g_walked = kWalkNext;
uint32_t g_gen = 0;
uint32_t g_started = 0;
std::unordered_map<std::wstring, std::vector<ue_wrap::CachedObjRef>> g_attached;

void DropAttachment(void* glowstick) {
    for (auto it = g_attached.begin(); it != g_attached.end();) {
        std::vector<ue_wrap::CachedObjRef>& list = it->second;
        for (size_t i = 0; i < list.size();) {
            if (!list[i].Alive() || list[i].Is(glowstick)) list.erase(list.begin() + static_cast<long>(i));
            else ++i;
        }
        if (list.empty()) it = g_attached.erase(it);
        else ++it;
    }
}

}  // namespace

bool IsBound(const std::wstring& key) {
    void* actor = nullptr;
    return coop::prop_key_index::Resolve(key, &actor) == coop::prop_key_index::Read::Hit && actor && R::IsLive(actor);
}

void Forget(void* glowstick) {
    for (size_t i = 0; i < g_waiting.size();) {
        if (!g_waiting[i].glowstick.Alive() || g_waiting[i].glowstick.Is(glowstick))
            g_waiting.erase(g_waiting.begin() + static_cast<long>(i));
        else ++i;
    }
    DropAttachment(glowstick);
}

void Wait(void* glowstick, const std::wstring& key) {
    Forget(glowstick);
    g_waiting.emplace_back();
    g_waiting.back().glowstick.Set(glowstick);
    g_waiting.back().key = key;
    g_gen = ue_wrap::world_identity::Generation();
    g_walked = kWalkNext;
    ++g_started;
}

void Attach(void* glowstick, const std::wstring& key) {
    Forget(glowstick);
    g_attached[key].emplace_back();
    g_attached[key].back().Set(glowstick);
}

void TickWaits(ReadyFn ready) {
    if (g_waiting.empty()) return;
    if (ue_wrap::world_identity::Generation() != g_gen) {
        UE_LOGI("glowstick_struck: the world changed -- %zu waiting stick(s) dropped with it", g_waiting.size());
        g_waiting.clear();
        return;
    }
    // Nothing but the key index's own table moving can change an answer, so the walk runs when its Version has.
    const uint64_t version = coop::prop_key_index::Version();
    if (version == g_walked) return;
    g_walked = version;
    std::vector<void*> bound;
    for (size_t i = 0; i < g_waiting.size();) {
        void* const g = g_waiting[i].glowstick.Get();
        if (!g) {
            g_waiting.erase(g_waiting.begin() + static_cast<long>(i));
        } else if (IsBound(g_waiting[i].key)) {
            bound.push_back(g);
            g_waiting.erase(g_waiting.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    for (void* g : bound) ready(g);
}

bool AnyAttached() { return !g_attached.empty(); }

void TakeAttached(const std::wstring& key, std::vector<void*>& out) {
    const auto it = g_attached.find(key);
    if (it == g_attached.end()) return;
    for (const ue_wrap::CachedObjRef& r : it->second)
        if (void* g = r.Get()) out.push_back(g);
    g_attached.erase(it);
}

void Clear() {
    g_waiting.clear();
    g_attached.clear();
    g_walked = kWalkNext;
    g_started = 0;
}

uint32_t WaitsStarted() { return g_started; }

}  // namespace coop::glowstick_struck
