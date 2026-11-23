// coop/world/obelisk_sync.cpp -- see coop/world/obelisk_sync.h.
//
// obelisk_C (0.9.0n): its begin play, unless `fallen`, sets its event, raises its mesh and after 1 s plays its fall;
// the fall's end (a__FinishedFunc) lands it: camera shakes, the land particle, the radar point made important,
// `radialForce`'s impulse while `physEvent`, seventy `prop_C` rocks and a `prop_obelisk_C` born from its graph, the
// main player's punch, then `fallen` and its mesh hidden. A client's copy keeps what is its own player's, and loses the
// push and the births.

#include "coop/world/obelisk_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coop::obelisk_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kClass = L"obelisk_C";
UE_FACT_FN(kRowBeginPlay, "obelisk_sync", L"obelisk_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowBeginDeferredSpawn, "obelisk_sync", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
constexpr int kTag = 0x4F42454C;   // 'OBEL'

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
bool g_hooked = false;
std::atomic<void*> g_copy{nullptr};   // CLIENT: the copy this lane spawned
float g_yaw = 0.f;                    // CLIENT: the host's obelisk's yaw, which its cue 108 brings before its place
std::mutex g_bornMx;
std::vector<void*> g_born;            // CLIENT: what the copy's landing made, ended next tick

bool ReadBool(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    out = (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
    return true;
}

void WriteBool(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

// HOST: the obelisk's begin play; one that has not fallen is about to fall.
void OnBeginPlayPost(const sg::Call& call) {
    if (call.tag != kTag) return;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host || !call.object) return;
    bool fallen = false;
    if (ReadBool(call.object, L"fallen", fallen) && fallen) return;
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!ue_wrap::engine::TryGetActorLocation(call.object, at)) return;
    ue_wrap::engine::TryGetActorRotation(call.object, rot);
    // Its turn first, then its place, on the one ordered lane: the copy is made at both.
    N::EventCuePayload turn{};
    turn.cueId = kCueObeliskYaw;
    turn.x = rot.Yaw;
    N::EventCuePayload p{};
    p.cueId = kCueObelisk;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    const bool sent = s->SendReliable(N::ReliableKind::EventCue, &turn, sizeof(turn)) &&
                      s->SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    UE_LOGI("obelisk: the host's obelisk falls at (%.0f,%.0f,%.0f) yaw %.0f -- EventCue %s", at.X, at.Y, at.Z, rot.Yaw,
            sent ? "sent" : "not sent");
}

// Every deferred spawn: on a client, what its own copy's graph makes is queued to end, as the host's cross.
void OnBeginDeferred(void* /*context*/, void* srcObj, void* spawned) {
    void* const copy = g_copy.load(std::memory_order_acquire);
    if (!copy || srcObj != copy || !spawned) return;
    std::lock_guard<std::mutex> lk(g_bornMx);
    g_born.push_back(spawned);
}

void EndBorn() {
    std::vector<void*> born;
    {
        std::lock_guard<std::mutex> lk(g_bornMx);
        born.swap(g_born);
    }
    int ended = 0;
    for (void* o : born)
        if (o && R::IsLive(o) && ue_wrap::engine::DestroyActor(o)) ++ended;
    if (ended) UE_LOGI("obelisk: %d prop(s) this copy's landing made ended -- the host's cross", ended);
}

// CLIENT: a joiner's save also holds the host's obelisk, and its actor may be made after the host's begin play reached
// this world (the cue's check found none): while this world's copy stands, any other obelisk by it ends, the copy
// being the one that falls with the host's.
void EndTwins() {
    void* const copy = g_copy.load(std::memory_order_acquire);
    ue_wrap::FVector c{};
    if (!copy || !R::IsLive(copy) || !ue_wrap::engine::TryGetActorLocation(copy, c)) return;
    for (void* o : R::FindObjectsByClass(kClass)) {
        ue_wrap::FVector at{};
        if (o == copy || !o || !R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__") ||
            !ue_wrap::engine::TryGetActorLocation(o, at))
            continue;
        const float dx = at.X - c.X, dy = at.Y - c.Y;
        if (dx * dx + dy * dy < 500.f * 500.f && ue_wrap::engine::DestroyActor(o))
            UE_LOGI("obelisk: this world's own obelisk from its save ended -- the copy falls with the host's");
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_watched && (g_watched = sg::WatchName(kRowBeginPlay, kTag, nullptr, &OnBeginPlayPost)))
        UE_LOGI("obelisk: the obelisk's begin play is watched");
    if (!g_hooked && s->role() == N::Role::Client &&
        (g_hooked = ue_wrap::ufunction_hook::InstallPostHook(ue_wrap::facts::Resolve(kRowBeginDeferredSpawn),
                                                             &OnBeginDeferred)))
        UE_LOGI("obelisk: a client's copy's births are caught");
    EndBorn();
    if (s->role() == N::Role::Client) EndTwins();
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueObelisk && p.cueId != kCueObeliskYaw) return false;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Client) return true;
    if (p.cueId == kCueObeliskYaw) {
        g_yaw = p.x;
        return true;
    }
    for (void* o : R::FindObjectsByClass(kClass)) {
        ue_wrap::FVector at{};
        if (!o || !R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__") ||
            !ue_wrap::engine::TryGetActorLocation(o, at))
            continue;
        const float dx = at.X - p.x, dy = at.Y - p.y;
        if (dx * dx + dy * dy < 500.f * 500.f) return true;   // this world has it already (a joiner's save)
    }
    void* cls = ue_wrap::object_index::ClassByName(kClass);
    if (!cls) cls = R::FindClass(kClass);
    // Turned as the host's before its begin play, which aims its mesh and its sign by its own turn; and its push off,
    // since the host's push moves the props, whose drive reaches this world.
    const ue_wrap::FVector at{p.x, p.y, p.z};
    const ue_wrap::FRotator rot{0.f, g_yaw, 0.f};
    void* const copy = cls ? ue_wrap::engine::BeginDeferredSpawn(cls, at, rot) : nullptr;
    if (copy) {
        WriteBool(copy, L"physEvent", false);
        ue_wrap::engine::FinishDeferredSpawn(copy, at, rot);
    }
    if (!copy || !R::IsLive(copy)) {
        UE_LOGW("obelisk: the host's obelisk falls, and this world could not make a copy");
        return true;
    }
    g_copy.store(copy, std::memory_order_release);
    UE_LOGI("obelisk: the host's obelisk falls here too, at (%.0f,%.0f,%.0f) yaw %.0f", p.x, p.y, p.z, g_yaw);
    return true;
}

void OnDisconnect() {
    g_copy.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_bornMx);
    g_born.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::obelisk_sync
