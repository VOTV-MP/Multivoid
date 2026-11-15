// coop/world/solto_sync.cpp -- see coop/world/solto_sync.h.
//
// From soltomiaCleaning_C's graph (0.10.0). The box overlap, once: the squeak on, the actor shown, the particles on,
// the ATV put on the craft with its physics off, the door closed and three doors jammed, `active` true, talking().
// `opened` (bound to the door's dooropened in the begin play): while `active`, 0.5 s on, the leaving montage, whose
// notifies swap the face and play a line (face1, face2) and at `throw` hide the mesh, mint a prop_sponge_solto_C and
// throw it, give the ATV its physics back and stop the squeak and the particles; the montage's end destroys the actor.

#include "coop/world/solto_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::solto_sync {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
using Clock = std::chrono::steady_clock;

constexpr const wchar_t* kClass = L"soltomiaCleaning_C";

UE_FACT_FN(kRowBoxOverlap, "solto_sync", L"soltomiaCleaning_C",
           L"BndEvt__soltomiaCleaning_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowOpened, "solto_sync", L"soltomiaCleaning_C", L"opened", Declared);
UE_FACT_FN(kRowBeginDeferred, "solto_sync", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
constexpr int kTagOverlap = 0x534C5430;  // 'SLT0'
constexpr int kTagOpened  = 0x534C5431;  // 'SLT1'

constexpr long long kHoldMs = 30000;     // how long a cue waits for its copy

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;

// HOST: the craft whose start, and whose leave, was sent (one craft at a time; a new one starts again).
void*   g_hostCraft = nullptr;
int32_t g_hostIdx = -1;
bool    g_hostStartSent = false, g_hostLeaveSent = false;

// CLIENT: a cue waiting for the copy, and the copy whose montage is playing (its sponge is refused).
bool      g_holdStart = false, g_holdLeave = false;
Clock::time_point g_holdSince{};
void*     g_leaving = nullptr;
int32_t   g_leavingIdx = -1;
void*     g_hookFn = nullptr;
bool      g_hooked = false, g_armed = false;

bool ReadFlag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void WriteFlag(void* obj, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return;
    uint8_t* b = static_cast<uint8_t*>(obj) + off;
    *b = on ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
}

void* Member(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* m = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off);
    return (m && R::IsLive(m)) ? m : nullptr;
}

// A verb of no arguments, or of one bool, on `obj`.
bool CallVerb(void* obj, const wchar_t* fnName, const wchar_t* boolArg = nullptr, bool value = false) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || (boolArg && !f.Set<bool>(boolArg, value))) return false;
    return ue_wrap::Call(obj, f);
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

coop::net::Session* Host() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->role() == coop::net::Role::Host) ? s : nullptr;
}

void TrackHostCraft(void* craft) {
    const int32_t idx = R::InternalIndexOf(craft);
    if (craft == g_hostCraft && idx == g_hostIdx && R::IsLiveByIndex(g_hostCraft, g_hostIdx)) return;
    g_hostCraft = craft;
    g_hostIdx = idx;
    g_hostStartSent = g_hostLeaveSent = false;
}

bool SendCue(coop::net::Session* s, uint32_t id, void* craft, int onlySlot = -1) {
    ue_wrap::FVector at{};
    E::TryGetActorLocation(craft, at);
    coop::net::EventCuePayload p{};
    p.cueId = id;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    if (!s->connected()) return false;
    return onlySlot >= 0 ? s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::EventCue, &p, sizeof(p))
                         : s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
}

// The box overlap ran: the first one that left the craft `active` is the start.
void OnOverlapPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object || !R::IsLive(call.object) || !ReadFlag(call.object, L"active")) return;
    TrackHostCraft(call.object);
    if (g_hostStartSent) return;
    g_hostStartSent = true;
    const bool sent = SendCue(s, kCueStart, call.object);
    UE_LOGI("solto: the craft is at work -- %s", sent ? "start cue sent" : "recorded only, no peer to send the cue to");
}

// The door was opened while the craft was at work: its leaving montage starts half a second on.
void OnOpenedPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object || !R::IsLive(call.object) || !ReadFlag(call.object, L"active")) return;
    TrackHostCraft(call.object);
    if (g_hostLeaveSent) return;
    g_hostLeaveSent = true;
    const bool sent = SendCue(s, kCueLeave, call.object);
    UE_LOGI("solto: the craft leaves -- %s", sent ? "leave cue sent" : "no peer to send the cue to");
}

// ---- CLIENT --------------------------------------------------------------------------------------------------------

SG::Verdict OnBeginDeferredPre(void* /*context*/, void* source, const uint8_t* /*parms*/);

void Disarm() {
    if (g_armed && UH::SetArmed(g_hookFn, &OnBeginDeferredPre, false)) g_armed = false;
    g_leaving = nullptr;
    g_leavingIdx = -1;
}

// While a copy's leaving montage plays: its own sponge is refused, the host's being the prop every peer has.
SG::Verdict OnBeginDeferredPre(void* /*context*/, void* source, const uint8_t* /*parms*/) {
    if (!g_leaving) return SG::Verdict::Run;
    if (!R::IsLiveByIndex(g_leaving, g_leavingIdx)) {
        Disarm();
        return SG::Verdict::Run;
    }
    if (source != g_leaving || !coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    UE_LOGI("solto: the copy's own sponge is refused -- the host's is the prop");
    return SG::Verdict::Cancel;
}

void ArmSpongeRefusal(void* copy) {
    if (!g_hooked) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
        if (!fn || !UH::InstallPreHook(fn, &OnBeginDeferredPre, /*armed=*/false)) {
            UE_LOGW("solto: the deferred spawn's pre hook did not install -- the copy mints a sponge beside the host's");
            return;
        }
        g_hooked = true;
        g_hookFn = fn.Fn();
    }
    g_leaving = copy;
    g_leavingIdx = R::InternalIndexOf(copy);
    g_armed = UH::SetArmed(g_hookFn, &OnBeginDeferredPre, true);
}

// The copy shown and its own squeak, particles and talk loop started, as the box overlap does on the host.
bool StartCopy(void* copy) {
    E::SetActorHiddenInGame(copy, false);
    const bool squeak = CallVerb(Member(copy, L"squeak"), L"Activate", L"bReset", false);
    const bool particles = CallVerb(Member(copy, L"ParticleSystem"), L"Activate", L"bReset", true);
    const bool already = ReadFlag(copy, L"active");
    WriteFlag(copy, L"active", true);
    const bool talk = already || CallVerb(copy, L"talking");
    UE_LOGI("solto: the host's craft is at work -- this peer's copy is shown (squeak %d, particles %d, talk %d)",
            squeak ? 1 : 0, particles ? 1 : 0, talk ? 1 : 0);
    return true;
}

// The copy's own `opened`: the montage, its faces and lines, the mesh hidden at the throw, and its own end.
bool LeaveCopy(void* copy) {
    if (!ReadFlag(copy, L"active")) StartCopy(copy);
    ArmSpongeRefusal(copy);
    const bool ran = CallVerb(copy, L"opened");
    UE_LOGI("solto: the host's craft leaves -- this peer's copy plays its leaving: %d", ran ? 1 : 0);
    return ran;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    const bool a = SG::WatchName(kRowBoxOverlap, kTagOverlap, nullptr, &OnOverlapPost);
    const bool b = SG::WatchName(kRowOpened, kTagOpened, nullptr, &OnOpenedPost);
    if (a && b)
        UE_LOGI("solto: the craft's start and leave are seen at its box overlap and its `opened` (script-gate watches)");
    else
        UE_LOGE("solto: a watch was refused (overlap %d, opened %d) -- a client's copy of the craft stays as it was born",
                a ? 1 : 0, b ? 1 : 0);
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueStart && p.cueId != kCueLeave) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return true;
    if (p.cueId == kCueStart) g_holdStart = true;
    else g_holdLeave = true;
    g_holdSince = Clock::now();
    Tick();
    return true;
}

void Tick() {
    if (!g_holdStart && !g_holdLeave) return;
    void* copy = ue_wrap::world_singleton::Find(kClass);
    if (!copy || !R::IsLive(copy)) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_holdSince).count();
        if (waited > kHoldMs) {
            UE_LOGW("solto: no copy of the craft appeared in %lld s -- the host's cue is dropped", kHoldMs / 1000);
            g_holdStart = g_holdLeave = false;
        }
        return;
    }
    if (g_holdStart) StartCopy(copy);
    if (g_holdLeave) LeaveCopy(copy);
    g_holdStart = g_holdLeave = false;
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = Host();
    if (!s) return;
    // The craft is read from the world, not from what the watches remembered: the last peer leaving clears that (OnDisconnect)
    // while the host's craft works on, and the next joiner's copy would stay hidden.
    void* craft = ue_wrap::world_singleton::Find(kClass);
    if (!craft || !R::IsLive(craft) || !ReadFlag(craft, L"active")) return;
    TrackHostCraft(craft);
    if (g_hostLeaveSent) return;
    g_hostStartSent = true;
    const bool sent = SendCue(s, kCueStart, craft, slot);
    UE_LOGI("solto: the craft is at work -> joiner slot %d: %d", slot, sent ? 1 : 0);
}

void OnDisconnect() {
    Disarm();
    g_holdStart = g_holdLeave = false;
    g_hostCraft = nullptr;
    g_hostIdx = -1;
    g_hostStartSent = g_hostLeaveSent = false;
}

}  // namespace coop::solto_sync
