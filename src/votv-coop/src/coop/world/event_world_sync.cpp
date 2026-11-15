// coop/world/event_world_sync.cpp -- see coop/world/event_world_sync.h. What each graph does, from the 0.10.0 bytecode:
//   newsky_C.tp: setEvent; earthTP_in; 20 s; light_blackhole and skyVisibility on; 6 s; a mail; 60 s; earthTP_out; 20 s; unless
//     the permanent-black-hole rule holds, both hidden, setEvent(false), two more mails. Each delay is one latent action.
//   arirShipAppear_C.appear(dir): setEvent, `visible`, the radar point, the fade timeline; its tick hides it 1 s after the
//     local camera sees it or the sun is up.
//   lakeglow_C.setActive: mist, loop, splash, light and beam follow `isActive` (written by ticker_lakeMonsterT).
//   radiotowerPoof_C: hides the level's radio tower once the local camera has not seen it, then shows it and dies likewise.
//   analogDScreenTest_C: redPhoneEventTimer rolls 2.5 % and startRinging rings every 3.5 s until the local camera comes
//     within 350 of the panel (tryRinging).
//   lockerCorpse_C: begin play traces a blood decal and sprays 25 rounds of gibs.
//   locker_C.open: with `head` set mints a lockerguy_C; ticker_lockerHead sets `head` (8 %) and mints a lockerCorpse_C (1 %).
//   event_fleshRain_C: after 5 s mints a prop_garbageClump_C every 0.2 s around player 0's camera.
//   midasUfoTest_C: setActive(true) (ufo_midas, through its trigger's runTrigger) shows it and zeroes `a`; its tick advances `a`
//     along the spline and ends in setActive(false) at 1.

#include "coop/world/event_world_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::event_world_sync {
namespace {

namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace SKY = ue_wrap::skysphere;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowLakeSetActive, "event_world_sync", L"lakeglow_C", L"setActive", Declared);
UE_FACT_FN(kRowShipAppear, "event_world_sync", L"arirShipAppear_C", L"appear", Declared);
UE_FACT_FN(kRowPoofBeginPlay, "event_world_sync", L"radiotowerPoof_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowRainBeginPlay, "event_world_sync", L"event_fleshRain_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowStartRinging, "event_world_sync", L"analogDScreenTest_C", L"startRinging", Declared);
UE_FACT_FN(kRowCorpseBeginPlay, "event_world_sync", L"lockerCorpse_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowMidasSetActive, "event_world_sync", L"midasUfoTest_C", L"setActive", Declared);
UE_FACT_FN(kRowLockerTicker, "event_world_sync", L"mainGamemode_C", L"ticker_lockerHead", Declared);
UE_FACT_FN(kRowPlaySound2D, "event_world_sync", L"GameplayStatics", L"PlaySound2D", Declared);
UE_FACT_FN(kRowBeginDeferred, "event_world_sync", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
constexpr int kTagLake = 0x45575330, kTagShip = 0x45575331, kTagPoof = 0x45575332, kTagRain = 0x45575333,
              kTagRing = 0x45575334, kTagLocker = 0x45575335, kTagCorpse = 0x45575336, kTagMidas = 0x45575337;  // 'EWS0'...

constexpr const wchar_t* kSoundIn = L"/Game/audio/effects/earthTP_in.earthTP_in";
constexpr const wchar_t* kSoundOut = L"/Game/audio/effects/earthTP_out.earthTP_out";
constexpr const wchar_t* kPoofPath = L"/Game/objects/misc/radiotowerPoof.radiotowerPoof_C";
constexpr const wchar_t* kCorpsePath = L"/Game/objects/misc/lockerCorpse.lockerCorpse_C";
constexpr const wchar_t* kRainPath = L"/Game/objects/event_fleshRain.event_fleshRain_C";
constexpr long long kHoldMs = 30000;     // how long a cue waits for its target
constexpr float kLockerReach = 250.f;    // how far a client's locker may stand from the host's and still be it

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
std::atomic<unsigned> g_sent{0};

// HOST: the last state sent of the two lasting flags, so each edge is sent once; the lockers announced.
bool g_lakeSent = false, g_shipSent = false;
std::vector<std::pair<void*, int32_t>> g_headSent;
bool g_soundHooked = false, g_soundArmed = false;
void* g_soundFn = nullptr;
int32_t g_offSound = -1;

// CLIENT: cues waiting for their target, the meat-rain refusal, and the spawn of our own that it must let through.
struct Pending { uint32_t id; float x, y, z; Clock::time_point since; };
std::vector<Pending> g_pending;
bool g_rainHooked = false, g_rainArmed = false, g_ownSpawn = false, g_rainNameReady = false;
void* g_rainFn = nullptr;
int32_t g_offActorClass = -1;
R::FName g_rainName{};
uint64_t g_rainRefused = 0;

coop::net::Session* Host() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->role() == coop::net::Role::Host) ? s : nullptr;
}

bool ReadFlag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

bool WriteFlag(void* obj, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return false;
    uint8_t* b = static_cast<uint8_t*>(obj) + off;
    *b = on ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    return true;
}

// The first live instance of `cls` (an exact class name) that is no class default object, or null.
void* LiveInstance(const wchar_t* cls) {
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

bool CallVerb(void* obj, const wchar_t* fnName, const wchar_t* boolArg = nullptr, bool value = false) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || (boolArg && !f.Set<bool>(boolArg, value))) return false;
    return ue_wrap::Call(obj, f);
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

bool SendCue(coop::net::Session* s, uint32_t id, float x, float y, float z, int onlySlot = -1) {
    if (!s->connected()) return false;
    coop::net::EventCuePayload p{};
    p.cueId = id;
    p.x = x;
    p.y = y;
    p.z = z;
    const bool ok = onlySlot >= 0 ? s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::EventCue, &p, sizeof(p))
                                  : s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
    g_sent.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

// A float member of `obj`, or `fallback`; and a write of one.
float ReadFloat(void* obj, const wchar_t* name, float fallback) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    float v = fallback;
    if (off >= 0) std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v;
}

bool WriteFloat(void* obj, const wchar_t* name, float v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(v));
    return true;
}

// The actor's place, or the origin.
ue_wrap::FVector PlaceOf(void* actor) {
    ue_wrap::FVector at{};
    if (actor) E::TryGetActorLocation(actor, at);
    return at;
}

void OnLakePost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object || !R::IsLive(call.object)) return;
    const bool on = ReadFlag(call.object, L"isActive");
    if (on == g_lakeSent) return;
    g_lakeSent = on;
    const bool sent = SendCue(s, on ? kCueLakeOn : kCueLakeOff, 0.f, 0.f, 0.f);
    UE_LOGI("event_world: the lake glow is %s on the host -- cue %s", on ? "lit" : "out", sent ? "sent" : "recorded only, no peer");
}

void OnShipPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object || !R::IsLive(call.object)) return;
    const bool on = ReadFlag(call.object, L"visible");
    if (on == g_shipSent) return;
    g_shipSent = on;
    const ue_wrap::FVector at = PlaceOf(call.object);
    const bool sent = SendCue(s, on ? kCueShipOn : kCueShipOff, at.X, at.Y, at.Z);
    UE_LOGI("event_world: the Day-13 ship tower is %s on the host -- cue %s", on ? "shown" : "hidden",
            sent ? "sent" : "recorded only, no peer");
}

void OnPoofPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object) return;
    const bool sent = SendCue(s, kCuePoof, 0.f, 0.f, 0.f);
    UE_LOGI("event_world: a radio-tower vanish began on the host -- cue %s", sent ? "sent" : "recorded only, no peer");
}

void OnRainPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object) return;
    const bool sent = SendCue(s, kCueMeatRain, 0.f, 0.f, 0.f);
    UE_LOGI("event_world: a meat rain began on the host -- cue %s", sent ? "sent" : "recorded only, no peer");
}

void OnCorpsePost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object) return;
    const ue_wrap::FVector at = PlaceOf(call.object);
    const bool sent = SendCue(s, kCueLockerCorpse, at.X, at.Y, at.Z);
    UE_LOGI("event_world: a locker's corpse began at (%.0f, %.0f, %.0f) on the host -- cue %s", at.X, at.Y, at.Z, sent ? "sent" : "recorded only, no peer");
}

// The Midas UFO was switched on or off: only an on is sent (the flyer's own tick ends it, here and on a client).
void OnMidasPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object || !R::IsLive(call.object) || !ReadFlag(call.object, L"active")) return;
    const float progress = ReadFloat(call.object, L"a", 0.f);
    const bool sent = SendCue(s, kCueMidasUfo, progress, 0.f, 0.f);
    UE_LOGI("event_world: the Midas UFO took off on the host (progress %.3f) -- cue %s", progress, sent ? "sent" : "recorded only, no peer");
}

void OnRingPost(const SG::Call& call) {
    auto* s = Host();
    if (!s || !call.object) return;
    const bool sent = SendCue(s, kCueRing, 0.f, 0.f, 0.f);
    UE_LOGI("event_world: the red phone began to ring on the host -- cue %s", sent ? "sent" : "recorded only, no peer");
}

// Every locker that holds a head and was not announced; with `onlySlot`, every one to that slot. A locker whose head is gone
// (opened) leaves the announced list, so a head set on it again is a new one.
void SendHeads(coop::net::Session* s, int onlySlot) {
    std::vector<std::pair<void*, int32_t>> still;
    for (void* locker : R::FindObjectsByClass(L"locker_C")) {
        if (!locker || !R::IsLive(locker) || !ReadFlag(locker, L"head")) continue;
        const int32_t idx = R::InternalIndexOf(locker);
        still.emplace_back(locker, idx);
        bool known = false;
        for (const auto& k : g_headSent) known = known || (k.first == locker && k.second == idx);
        if (known && onlySlot < 0) continue;
        const ue_wrap::FVector at = PlaceOf(locker);
        const bool sent = SendCue(s, kCueLockerHead, at.X, at.Y, at.Z, onlySlot);
        UE_LOGI("event_world: a locker at (%.0f, %.0f, %.0f) holds a head on the host -- cue %s", at.X, at.Y, at.Z,
                sent ? "sent" : "recorded only, no peer");
    }
    if (onlySlot < 0) g_headSent.swap(still);
}

void OnLockerTickerPost(const SG::Call&) {
    if (auto* s = Host()) SendHeads(s, -1);
}

// The sound seam: newsky's own PlaySound2D of the two Black Hole Sun sounds, which no state carries. Only a call from the sky's
// own graph is looked at (a pointer compare), and then the sound's name.
SG::Verdict OnPlaySound2DPre(void* /*context*/, void* source, const uint8_t* parms) {
    auto* s = Host();
    if (!s || !source || !parms || g_offSound < 0 || source != SKY::Sky()) return SG::Verdict::Run;
    void* sound = *reinterpret_cast<void* const*>(parms + g_offSound);
    if (!sound) return SG::Verdict::Run;
    const R::FName& n = R::NameOf(sound);
    const bool in = R::NameEquals(n, L"earthTP_in");
    if (!in && !R::NameEquals(n, L"earthTP_out")) return SG::Verdict::Run;
    const bool sent = SendCue(s, in ? kCueTpIn : kCueTpOut, 0.f, 0.f, 0.f);
    UE_LOGI("event_world: the sky played the Black Hole Sun's '%s' sound -- cue %s", in ? "in" : "out",
            sent ? "sent" : "recorded only, no peer");
    return SG::Verdict::Run;
}

void ArmSoundSeam(bool want) {
    if (want && !g_soundHooked) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySound2D);
        if (!fn || !UH::InstallPreHook(fn, &OnPlaySound2DPre, /*armed=*/false)) return;  // tried again at the next tick
        g_soundHooked = true;
        g_soundFn = fn.Fn();
        g_offSound = R::FindParamOffset(g_soundFn, L"Sound");
        if (g_offSound < 0) UE_LOGW("event_world: PlaySound2D has no `Sound` parameter by that name -- the Black Hole Sun's sounds are not sent");
    }
    if (g_soundHooked && g_offSound >= 0 && want != g_soundArmed) g_soundArmed = UH::SetArmed(g_soundFn, &OnPlaySound2DPre, want) ? want : g_soundArmed;
}

// ---- CLIENT --------------------------------------------------------------------------------------------------------

// The meat rain's roll sits in the day cycle's big graph, where it mints the actor by an EX_CallMath deferred spawn: refused at
// the native seam while a client's session runs, by the class the call names. Our own spawn of the host's rain passes.
SG::Verdict OnBeginDeferredPre(void* /*context*/, void* /*source*/, const uint8_t* parms) {
    if (g_ownSpawn || !parms || g_offActorClass < 0 || !g_rainNameReady) return SG::Verdict::Run;
    void* cls = *reinterpret_cast<void* const*>(parms + g_offActorClass);
    if (!cls) return SG::Verdict::Run;
    const R::FName& n = R::NameOf(cls);
    if (n.ComparisonIndex != g_rainName.ComparisonIndex || n.Number != g_rainName.Number) return SG::Verdict::Run;
    if ((++g_rainRefused & (g_rainRefused - 1)) == 0)
        UE_LOGI("event_world: a client's own meat-rain roll is refused -- the host's is the rain (refusal #%llu)",
                static_cast<unsigned long long>(g_rainRefused));
    return SG::Verdict::Cancel;
}

void ArmRainRefusal(bool want) {
    if (!g_rainNameReady && ue_wrap::fname_utils::Resolved()) {
        g_rainName = ue_wrap::fname_utils::StringToFName(L"event_fleshRain_C");
        g_rainNameReady = true;
    }
    if (want && !g_rainHooked) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
        if (!fn || !UH::InstallPreHook(fn, &OnBeginDeferredPre, /*armed=*/false)) return;
        g_rainHooked = true;
        g_rainFn = fn.Fn();
        g_offActorClass = R::FindParamOffset(g_rainFn, L"ActorClass");
        if (g_offActorClass < 0) UE_LOGW("event_world: the deferred spawn has no `ActorClass` parameter by that name -- a client's own meat rain is not refused");
    }
    if (g_rainHooked && g_offActorClass >= 0 && want != g_rainArmed) g_rainArmed = UH::SetArmed(g_rainFn, &OnBeginDeferredPre, want) ? want : g_rainArmed;
}

bool PlaySound(const wchar_t* path) {
    void* sound = ue_wrap::asset_load::LoadObjectByPath(path);
    void* cdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* fn = cdo ? R::FindFunction(R::ClassOf(cdo), L"PlaySound2D") : nullptr;
    void* ctx = coop::players::Registry::Get().Local();
    if (!sound || !fn || !ctx) {
        UE_LOGW("event_world: the Black Hole Sun's sound %ls was not played (sound %p, verb %p, world context %p)", path, sound, fn, ctx);
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"WorldContextObject", ctx);
    f.Set<void*>(L"Sound", sound);
    f.Set<float>(L"VolumeMultiplier", 1.5f);
    f.Set<float>(L"PitchMultiplier", 0.4f);
    f.Set<float>(L"StartTime", 0.f);
    f.Set<bool>(L"bIsUISound", true);
    const bool ran = ue_wrap::Call(cdo, f);
    UE_LOGI("event_world: the host's Black Hole Sun sound %ls played here: %d", path, ran ? 1 : 0);
    return ran;
}

// One of our own, at the origin as the game's roll places it, when this peer has none of the class live.
bool SpawnOwn(const wchar_t* className, const wchar_t* path) {
    if (LiveInstance(className)) return true;
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    if (!cls) {
        UE_LOGW("event_world: %ls did not load by %ls -- the host's event is not shown here", className, path);
        return true;
    }
    g_ownSpawn = true;
    void* actor = E::SpawnActor(cls, {0.f, 0.f, 0.f});
    g_ownSpawn = false;
    UE_LOGI("event_world: this peer's own %ls spawned (%p)", className, actor);
    return true;
}

// The locker of this peer standing nearest the host's, within reach.
void* NearestLocker(float x, float y, float z) {
    void* best = nullptr;
    float bestD = kLockerReach * kLockerReach;
    for (void* locker : R::FindObjectsByClass(L"locker_C")) {
        if (!locker || !R::IsLive(locker) || R::NameStartsWith(R::NameOf(locker), L"Default__")) continue;
        const ue_wrap::FVector at = PlaceOf(locker);
        const float d = (at.X - x) * (at.X - x) + (at.Y - y) * (at.Y - y) + (at.Z - z) * (at.Z - z);
        if (d <= bestD) { bestD = d; best = locker; }
    }
    return best;
}

// Whether the panel's red-phone timer handle is live: an FTimerHandle is one 64-bit id, 0 when invalid.
bool RingLive(void* panel) {
    const int32_t off = panel ? R::FindPropertyOffset(R::ClassOf(panel), L"timerHandle_redphone") : -1;
    uint64_t handle = 0;
    if (off >= 0) std::memcpy(&handle, static_cast<const uint8_t*>(panel) + off, sizeof(handle));
    return handle != 0;
}

// Runs one cue on this peer: true when it is done (or hopeless), false to be tried again once its target is born.
bool Apply(const Pending& c) {
    switch (c.id) {
    case kCueTpIn:
        PlaySound(kSoundIn);
        return true;
    case kCueTpOut:
        PlaySound(kSoundOut);
        return true;
    case kCueShipOn:
    case kCueShipOff: {
        void* tower = ue_wrap::world_singleton::Find(L"arirShipAppear_C");
        if (!tower) return false;
        const bool want = c.id == kCueShipOn;
        // Its own tick may have hidden it already (the local camera looked at it): a second `appear(false)` would count the
        // event down twice, so only a change runs.
        const bool ran = ReadFlag(tower, L"visible") != want && CallVerb(tower, L"appear", L"dir", want);
        UE_LOGI("event_world: the host's ship tower %s -- this peer's own tower %s", want ? "shown" : "hidden", ran ? "followed" : "was already so");
        return true;
    }
    case kCueLakeOn:
    case kCueLakeOff: {
        void* lake = ue_wrap::world_singleton::Find(L"lakeglow_C");
        if (!lake) return false;
        const bool want = c.id == kCueLakeOn;
        const bool ran = ReadFlag(lake, L"isActive") != want && CallVerb(lake, L"setActive", L"activate", want);
        UE_LOGI("event_world: the host's lake glow is %s -- this peer's own lake %s", want ? "lit" : "out", ran ? "followed" : "was already so");
        return true;
    }
    case kCuePoof:
        return SpawnOwn(L"radiotowerPoof_C", kPoofPath);
    case kCueMeatRain:
        return SpawnOwn(L"event_fleshRain_C", kRainPath);
    case kCueLockerCorpse: {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kCorpsePath);
        void* actor = cls ? E::SpawnActor(cls, {c.x, c.y, c.z}) : nullptr;
        UE_LOGI("event_world: the host's locker corpse at (%.0f, %.0f, %.0f) -- this peer's own spawned (%p)", c.x, c.y, c.z, actor);
        return true;
    }
    case kCueMidasUfo: {
        void* ufo = ue_wrap::world_singleton::Find(L"midasUfoTest_C");
        if (!ufo) return false;
        const bool started = !ReadFlag(ufo, L"active") && CallVerb(ufo, L"setActive", L"active", true);
        // A joiner during the flight: the host's progress along the spline. A start is sent at 0, which the call just wrote.
        const bool placed = (started || ReadFlag(ufo, L"active")) && c.x > 0.f && c.x < 1.f && WriteFloat(ufo, L"a", c.x);
        UE_LOGI("event_world: the host's Midas UFO is flying (progress %.3f) -- this peer's own flyer started %d, progress set %d", c.x,
                started ? 1 : 0, placed ? 1 : 0);
        return true;
    }
    case kCueRing: {
        void* panel = ue_wrap::world_singleton::Find(L"analogDScreenTest_C");
        if (!panel) return false;
        const bool ran = !RingLive(panel) && CallVerb(panel, L"startRinging");
        UE_LOGI("event_world: the host's red phone rings -- this peer's own panel %s", ran ? "started its ring" : "was already ringing");
        return true;
    }
    case kCueLockerHead: {
        void* locker = NearestLocker(c.x, c.y, c.z);
        if (!locker) return false;
        const bool wrote = WriteFlag(locker, L"head", true);
        UE_LOGI("event_world: the host's locker at (%.0f, %.0f, %.0f) holds a head -- this peer's nearest locker %p: %d", c.x, c.y, c.z, locker, wrote ? 1 : 0);
        return true;
    }
    default:
        return true;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    int ok = 0;
    ok += SG::WatchName(kRowLakeSetActive, kTagLake, nullptr, &OnLakePost) ? 1 : 0;
    ok += SG::WatchName(kRowShipAppear, kTagShip, nullptr, &OnShipPost) ? 1 : 0;
    ok += SG::WatchName(kRowPoofBeginPlay, kTagPoof, nullptr, &OnPoofPost) ? 1 : 0;
    ok += SG::WatchName(kRowRainBeginPlay, kTagRain, nullptr, &OnRainPost) ? 1 : 0;
    ok += SG::WatchName(kRowStartRinging, kTagRing, nullptr, &OnRingPost) ? 1 : 0;
    ok += SG::WatchName(kRowLockerTicker, kTagLocker, nullptr, &OnLockerTickerPost) ? 1 : 0;
    ok += SG::WatchName(kRowCorpseBeginPlay, kTagCorpse, nullptr, &OnCorpsePost) ? 1 : 0;
    ok += SG::WatchName(kRowMidasSetActive, kTagMidas, nullptr, &OnMidasPost) ? 1 : 0;
    if (ok == 8)
        UE_LOGI("event_world: the lake, the ship tower, the radio-tower vanish, the meat rain, the red phone, the locker head, the locker corpse and the Midas UFO are seen (script-gate watches)");
    else
        UE_LOGE("event_world: %d of 8 watches were refused -- a client does not see that event of the host's", 8 - ok);
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId < kCueTpIn || p.cueId > 149) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return true;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return true;
    if (g_pending.size() < 32) g_pending.push_back({p.cueId, p.x, p.y, p.z, Clock::now()});
    Tick();
    return true;
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    ArmSoundSeam(host && s->connected());
    ArmRainRefusal(!host && s->running());
    if (host || g_pending.empty()) return;
    for (size_t i = 0; i < g_pending.size();) {
        const Pending c = g_pending[i];
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - c.since).count();
        if (Apply(c)) {
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (waited > kHoldMs) {
            UE_LOGW("event_world: cue %u found no target on this peer in %lld s -- dropped", c.id, kHoldMs / 1000);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = Host();
    if (!s) return;
    int n = 0;
    if (void* lake = ue_wrap::world_singleton::Find(L"lakeglow_C"); lake && ReadFlag(lake, L"isActive")) {
        SendCue(s, kCueLakeOn, 0.f, 0.f, 0.f, slot);
        ++n;
    }
    if (void* tower = ue_wrap::world_singleton::Find(L"arirShipAppear_C"); tower && ReadFlag(tower, L"visible")) {
        const ue_wrap::FVector at = PlaceOf(tower);
        SendCue(s, kCueShipOn, at.X, at.Y, at.Z, slot);
        ++n;
    }
    if (LiveInstance(L"radiotowerPoof_C")) { SendCue(s, kCuePoof, 0.f, 0.f, 0.f, slot); ++n; }
    if (LiveInstance(L"event_fleshRain_C")) { SendCue(s, kCueMeatRain, 0.f, 0.f, 0.f, slot); ++n; }
    if (void* panel = ue_wrap::world_singleton::Find(L"analogDScreenTest_C"); panel && RingLive(panel)) {
        SendCue(s, kCueRing, 0.f, 0.f, 0.f, slot);
        ++n;
    }
    if (void* ufo = ue_wrap::world_singleton::Find(L"midasUfoTest_C"); ufo && ReadFlag(ufo, L"active")) {
        SendCue(s, kCueMidasUfo, ReadFloat(ufo, L"a", 0.f), 0.f, 0.f, slot);
        ++n;
    }
    for (void* corpse : R::FindObjectsByClass(L"lockerCorpse_C")) {
        if (!corpse || !R::IsLive(corpse) || R::NameStartsWith(R::NameOf(corpse), L"Default__")) continue;
        const ue_wrap::FVector at = PlaceOf(corpse);
        SendCue(s, kCueLockerCorpse, at.X, at.Y, at.Z, slot);
        ++n;
    }
    SendHeads(s, slot);
    UE_LOGI("event_world: joiner slot %d told of %d lasting event state(s) (and any lockers with a head)", slot, n);
}

void OnDisconnect() {
    ArmSoundSeam(false);
    ArmRainRefusal(false);
    g_pending.clear();
    g_headSent.clear();
    g_lakeSent = g_shipSent = false;
    g_rainRefused = 0;
    g_session.store(nullptr, std::memory_order_release);
}

unsigned SentCount() { return g_sent.load(std::memory_order_relaxed); }

}  // namespace coop::event_world_sync
