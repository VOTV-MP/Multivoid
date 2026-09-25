// coop/interactables/lightgroup_verbs.cpp -- see coop/interactables/lightgroup_verbs.h.

#include "coop/interactables/lightgroup_verbs.h"

#include "coop/interactables/interactable_sync.h"  // the group lane's key, its apply and the host's edge
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/lightswitch.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace coop::lightgroup_verbs {
namespace {

namespace LS = ue_wrap::lightswitch;
namespace sg = ue_wrap::script_gate;

// Every trigger class declares runTrigger, so the callbacks see a door's, a garage's and an
// eventer's too; each gates on the light root class first.
constexpr const wchar_t* kVerb = L"runTrigger";
constexpr int kTag = 0x4C475254;  // 'LGRT'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_announcedLive = false;

uint64_t g_refused = 0, g_edges = 0;
// A group's first refusal is said; the rest are counted. The set holds each group this client's own
// world tried to move, once.
std::unordered_set<std::wstring> g_saidRefused;

coop::net::Session* ConnectedAs(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected() && s->role() == role) ? s : nullptr;
}

// CLIENT, before the body.
sg::Verdict OnTriggerPre(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Client)) return sg::Verdict::Run;
    if (!call.object || !LS::IsLightRoot(call.object)) return sg::Verdict::Run;
    if (coop::interactable_sync::ApplyingLightGroup(call.object)) return sg::Verdict::Run;  // the lane's own apply
    const std::wstring key = coop::interactable_sync::LightGroupKey(call.object);
    if (key.empty()) return sg::Verdict::Run;  // no lane owns it: native
    ++g_refused;
    if (g_saidRefused.insert(key).second)
        UE_LOGI("[LIGHT-GROUP] client refused its own runTrigger on key='%ls' -- the host's copy moves this "
                "group (first refusal on this key; the rest are counted)", key.c_str());
    return sg::Verdict::Cancel;
}

// HOST, after the body.
void OnTriggerPost(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Host)) return;
    if (!call.object || !LS::IsLightRoot(call.object)) return;
    ++g_edges;
    coop::interactable_sync::OnLightGroupVerb(call.object);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watchInstalled) g_watchInstalled = sg::WatchName(kVerb, kTag, &OnTriggerPre, &OnTriggerPost);
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_announcedLive || !g_watchInstalled || !sg::NameWatchLive(kVerb, kTag)) return;
    g_announcedLive = true;
    UE_LOGI("[LIGHT-GROUP] the light group gate is live: runTrigger");
}

void OnDisconnect() {
    if (g_refused || g_edges)
        UE_LOGI("[LIGHT-GROUP] session end -- a client refused %llu local runTrigger call(s) on %zu group(s); "
                "the host saw %llu", static_cast<unsigned long long>(g_refused), g_saidRefused.size(),
                static_cast<unsigned long long>(g_edges));
    g_refused = g_edges = 0;
    g_saidRefused.clear();
}

}  // namespace coop::lightgroup_verbs
