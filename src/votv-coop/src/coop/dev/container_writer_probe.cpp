// coop/dev/container_writer_probe.cpp -- [dev] a container writer the lane never saw. See the header.
#include "coop/dev/container_writer_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "coop/session/host_world_settle.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace coop::dev::container_writer_probe {
namespace {

namespace CC = coop::props::container_contents_sync;

constexpr uint64_t kSweepMs = 2000;

struct Reading {
    uint64_t hash = 0;
    uint32_t epoch = 0;
};

std::map<uint32_t, Reading> g_last;   // the probe's own last reading per container
std::set<uint32_t> g_said;            // said once per container
uint64_t g_nextMs = 0;

// The client half: the (container, outermost script function) pairs already said, capped; past the cap the
// probe only counts.
constexpr size_t kMaxClientKeys = 256;
std::set<std::pair<uint32_t, void*>> g_clientSaid;
std::map<uint32_t, uint32_t> g_clientVerbs;   // verbs counted per container, capped at kMaxClientKeys containers
uint32_t g_clientOverCap = 0;
bool g_clientCapSaid = false;

void ReadOne(uint32_t eid, void*) {
    uint64_t hash = 0;
    uint32_t records = 0;
    if (!CC::ContentHashOf(eid, hash, records)) {   // not a settled reading: the next one primes again
        g_last.erase(eid);
        return;
    }
    const uint32_t epoch = CC::TouchEpochOf(eid);
    const auto it = g_last.find(eid);
    if (it != g_last.end() && it->second.hash != hash && it->second.epoch == epoch && g_said.insert(eid).second) {
        void* const actor = coop::element::LivePropActor(eid);
        UE_LOGW("container_writer_probe: eid=%u (%ls) changed with no host write seen", eid,
                actor ? ue_wrap::reflection::ClassNameOf(actor).c_str() : L"gone");
    }
    g_last[eid] = Reading{hash, epoch};
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::container_writer_probe);
    if (!on || !session || !session->running() || session->role() != coop::net::Role::Host) return;
    // A load and a thrown container's birth write containers with no verb: the sweep starts with the settled world.
    if (!coop::host_world_settle::Settled()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kSweepMs;
    CC::ForEachWorldContainer(&ReadOne, nullptr);
}

uint32_t ClientVerbsOf(uint32_t eid) {
    const auto it = g_clientVerbs.find(eid);
    return it == g_clientVerbs.end() ? 0 : it->second;
}

void OnClientVerb(uint32_t eid, void* owner, const ue_wrap::script_gate::Call& c, bool take) {
    if (g_clientVerbs.size() < kMaxClientKeys || g_clientVerbs.count(eid) != 0) ++g_clientVerbs[eid];
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::container_writer_probe);
    if (!on) return;
    void* const outer = ue_wrap::script_gate::OutermostFunction(c);
    if (g_clientSaid.count({eid, outer}) != 0) return;
    if (g_clientSaid.size() >= kMaxClientKeys) {
        ++g_clientOverCap;
        if (!g_clientCapSaid) {
            g_clientCapSaid = true;
            UE_LOGW("container_writer_probe: client probe capped at %zu pairs -- later ones are only counted",
                    kMaxClientKeys);
        }
        return;
    }
    g_clientSaid.insert({eid, outer});
    namespace R = ue_wrap::reflection;
    const std::wstring cls = owner ? R::ClassNameOf(owner) : std::wstring(L"?");
    const std::wstring from = outer ? R::ToString(R::NameOf(outer)) : std::wstring(L"?");
    if (take)
        UE_LOGW("container_writer_probe: client takeObj on eid=%u (%ls) from %ls -- not an asked act", eid,
                cls.c_str(), from.c_str());
    else
        UE_LOGI("container_writer_probe: client addObject on eid=%u (%ls) from %ls -- not an asked act", eid,
                cls.c_str(), from.c_str());
}

void OnDisconnect() {
    if (g_clientOverCap != 0)
        UE_LOGW("container_writer_probe: %u client verbs on pairs past the cap were counted and not said",
                g_clientOverCap);
    g_clientSaid.clear();
    g_clientVerbs.clear();
    g_clientOverCap = 0;
    g_clientCapSaid = false;
    g_last.clear();
    g_said.clear();
    g_nextMs = 0;
}

}  // namespace coop::dev::container_writer_probe
