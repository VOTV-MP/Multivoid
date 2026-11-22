// coop/world/host_event_presence.cpp -- see coop/world/host_event_presence.h.

#include "coop/world/host_event_presence.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/active_events.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::host_event_presence {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace AE = ue_wrap::active_events;

// The event classes whose actor stands on the host alone, so no client copy registers. A class gets a row when its
// event is measured host-only and its lane carries nothing that registers on a client.
constexpr const wchar_t* kHostOnly[] = {
    L"kocker_C",   // the vent knocker (day 12): coop/world/sound_relay carries its knocks, nothing its registration
};

std::atomic<N::Session*> g_session{nullptr};
bool g_sentPresent = false;   // HOST: what the clients were last told
bool g_registered = false;    // CLIENT: the stand-in is registered
void* g_standIn = nullptr;    // CLIENT: the eventer it registered

bool IsHostOnly(void* obj) {
    const std::wstring cls = R::ClassNameOf(obj);
    for (const wchar_t* c : kHostOnly)
        if (cls == c) return true;
    return false;
}

// HOST: whether any live registration in the game's own registry is a host-only class.
bool Present() {
    AE::Senders arr{};
    if (!AE::EnsureResolved() || !AE::ReadSenders(arr) || arr.num < 0 || arr.num > 4096 || !arr.data) return false;
    for (int32_t i = 0; i < arr.num; ++i)
        if (arr.data[i] && R::IsLive(arr.data[i]) && IsHostOnly(arr.data[i])) return true;
    return false;
}

N::EventCuePayload Cue(bool present) {
    N::EventCuePayload p{};
    p.cueId = kCueHostEvent;
    p.x = present ? 1.f : 0.f;
    return p;
}

// CLIENT: the game's own setEvent, for the stand-in.
bool SetEvent(void* sender, bool active) {
    void* const lib = R::FindClassDefaultObject(L"lib_C");
    void* const cls = ue_wrap::object_index::ClassByName(L"lib_C");
    ue_wrap::ParamFrame f(cls ? R::FindFunction(cls, L"setEvent") : nullptr);
    return lib && sender && f.valid() && f.Set<bool>(L"isEventActive", active) &&
           f.Set<bool>(L"deactivateAmbientTrack", false) && f.Set<void*>(L"__WorldContext", sender) &&
           ue_wrap::Call(lib, f);
}

void* Eventer() {
    for (void* o : R::FindObjectsByClass(L"trigger_eventer_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void OnHostEdge(void* sender, bool /*active*/) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host || !sender || !IsHostOnly(sender)) return;
    const bool present = Present();
    if (present == g_sentPresent) return;
    const N::EventCuePayload p = Cue(present);
    if (!s->SendReliable(N::ReliableKind::EventCue, &p, sizeof(p))) return;
    g_sentPresent = present;
    UE_LOGI("host_event: a host-only event %s -- EventCue %u sent", present ? "runs" : "ended", kCueHostEvent);
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host || !Present()) return;
    const N::EventCuePayload p = Cue(true);
    s->SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
    g_sentPresent = true;
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueHostEvent) return false;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Client) return true;
    const bool present = p.x > 0.5f;
    if (present == g_registered) return true;
    if (present) g_standIn = Eventer();
    const bool ran = SetEvent(g_standIn, present);
    if (ran) g_registered = present;
    UE_LOGI("host_event: the host's event %s here too (ran=%d)", present ? "runs" : "ended", ran ? 1 : 0);
    return true;
}

void OnDisconnect() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == N::Role::Client && g_registered && g_standIn && R::IsLive(g_standIn)) SetEvent(g_standIn, false);
    g_registered = false;
    g_standIn = nullptr;
    g_sentPresent = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::host_event_presence
