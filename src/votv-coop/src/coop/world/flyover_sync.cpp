// coop/world/flyover_sync.cpp -- see coop/world/flyover_sync.h.
//
// 0.9.0n. rozitBorg_C: its begin play sets its event; its tick adds dt/480 to `alpha`, lerps its `ships` from (+2e6,0,
// 1e5) to (-2e6,0,...), shakes the cameras near its billboard, and at alpha 1 clears its event and ends. morningUfo_C:
// its begin play sets its event and a looping timer on `chck`; its box (a mainPlayer_C) sets `fly` and shows it; `chck`
// traces from its `grayUfoSk` to a point in player 0's capsule and, on a hit of that player, sets `flyAway` 0.25 s on;
// its tick flies it to `flyTo`, or once `flyAway` to `flyAwayTo` and ends there. ufoDropper_C (actor_save_c): its begin
// play registers its save entry and a 10 s timer on `killKerfur` (a random kerfurOmega_C drops what it holds); its box,
// or `forceDeploy`, once, shows it, sets its event, starts its particles and plays `Timeline_0`, whose update lowers
// `grayUfoSk` and sets the `weight` its tick blinks the ceiling lights and the lamp posts by, whose `drop` spawns its
// `drop` class at its billboard, and whose end clears its event and ends it.

#include "coop/world/flyover_sync.h"

#include "coop/world/creature_event_relay.h"
#include "coop/world/world_actor_sync.h"
#include "coop/creatures/served_player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace coop::flyover_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace SP = coop::served_player;

constexpr const wchar_t* kBorg = L"rozitBorg_C";
constexpr const wchar_t* kMorning = L"morningUfo_C";
constexpr const wchar_t* kDropperKinds[] = {L"ufoDropper_body_C", L"ufoDropper_car_C"};

UE_FACT_FN(kRowMorningBox, "flyover_sync", L"morningUfo_C",
           L"BndEvt__morningUfo_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowDropBegin, "flyover_sync", L"ufoDropper_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowDropBox, "flyover_sync", L"ufoDropper_C",
           L"BndEvt__ufoDropper_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowDropForce, "flyover_sync", L"ufoDropper_C", L"forceDeploy", Declared);
UE_FACT_FN(kRowDropDrop, "flyover_sync", L"ufoDropper_C", L"Timeline_0__drop__EventFunc", Declared);
UE_FACT_FN(kRowDropEnd, "flyover_sync", L"ufoDropper_C", L"Timeline_0__FinishedFunc", Declared);
constexpr int kTagMorningBox = 0x464C4D42;  // 'FLMB'
constexpr int kTagDropBegin = 0x464C4442;   // 'FLDB'
constexpr int kTagDropBox = 0x464C4458;     // 'FLDX'
constexpr int kTagDropForce = 0x464C4446;   // 'FLDF'
constexpr int kTagDropDrop = 0x464C4444;    // 'FLDD'
constexpr int kTagDropEnd = 0x464C4445;     // 'FLDE'
constexpr uint64_t kServeMs = 200, kAlphaMs = 2000;

std::atomic<N::Session*> g_session{nullptr};
bool g_morningBox = false, g_dropBegin = false, g_dropBox = false, g_dropForce = false, g_dropDrop = false,
     g_dropEnd = false;
uint64_t g_nextServe = 0, g_nextAlpha = 0;
std::unordered_set<void*> g_sent;   // HOST: the UFOs and saucers whose show is sent
std::vector<ue_wrap::FVector> g_live;   // CLIENT: the host's saucers' places, as its join list sent them
uint64_t g_listDoneAt = 0;              // CLIENT: when the join list ended (0: not yet)
constexpr uint64_t kTwinWindowMs = 30000;   // CLIENT: from the list's end, how long its save's saucers are judged
constexpr float kListEndZ = -1.0e9f;        // the join list's end marker, a place no saucer stands at

bool IsRole(N::Role r) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == r;
}

int ReadBool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) != 0 ? 1 : 0;
}

void WriteBool(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

float* FloatField(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<uint8_t*>(obj) + off) : nullptr;
}

void* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v && R::IsLive(v) ? v : nullptr;
}

// A function of `obj` (its own or an ancestor's) with no parameters, or with its one bool parameter `bReset`.
bool Run(void* obj, const wchar_t* fnName, int reset = -1) {
    void* const fn = obj ? R::FindDispatchFunction(R::ClassOf(obj), fnName, nullptr) : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (reset >= 0) f.Set<bool>(L"bReset", reset != 0);
    return ue_wrap::Call(obj, f);
}

void Send(uint32_t cue, float x, float y, float z) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host) return;
    N::EventCuePayload p{};
    p.cueId = cue;
    p.x = x;
    p.y = y;
    p.z = z;
    s->SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
}

// The live actor of `cls` nearest `at` on this machine.
void* NearestOf(const wchar_t* cls, const ue_wrap::FVector& at, float& best) {
    void* found[16];
    const int32_t n = ue_wrap::world_instances::Find(cls, found, 16);
    void* pick = nullptr;
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::FVector p{};
        if (!found[i] || !R::IsLive(found[i]) || !ue_wrap::engine::TryGetActorLocation(found[i], p)) continue;
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < best) {
            best = d;
            pick = found[i];
        }
    }
    return pick;
}

// The nearest player body to `at`: 0 the host's own, else a client slot whose puppet has a body and a pose.
uint8_t NearestPlayer(const ue_wrap::FVector& at) {
    auto& reg = coop::players::Registry::Get();
    uint8_t best = 0;
    float bestD = 3.4e38f;
    ue_wrap::FVector p{};
    if (void* me = reg.Local(); me && ue_wrap::engine::TryGetActorLocation(me, p)) {
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        bestD = dx * dx + dy * dy + dz * dz;
    }
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        coop::RemotePlayer* const rp = reg.Puppet(slot);
        if (!rp || !rp->valid() || !rp->HasPose() || !rp->GetActor() || !rp->TryGetLocation(p)) continue;
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = slot;
        }
    }
    return best;
}

// CLIENT: what the host's world decides -- the UFO's box, the saucer's save entry, timer, box, deploy, drop and end.
sg::Verdict OnClientRefusePre(const sg::Call& call) {
    if (!IsRole(N::Role::Client)) return sg::Verdict::Run;
    switch (call.tag) {
        case kTagMorningBox: case kTagDropBegin: case kTagDropBox: case kTagDropForce: case kTagDropDrop:
        case kTagDropEnd: return sg::Verdict::Cancel;
        default: return sg::Verdict::Run;
    }
}

// HOST: a UFO or a saucer shown by its box's run or a deploy is sent, once.
void OnShowPost(const sg::Call& call) {
    if (!IsRole(N::Role::Host) || !call.object || ReadBool(call.object, L"bHidden") != 0) return;
    if (!g_sent.insert(call.object).second) return;
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(call.object, at)) return;
    const bool ufo = call.tag == kTagMorningBox;
    Send(ufo ? kCueMorningShow : kCueDropperDeploy, at.X, at.Y, at.Z);
    UE_LOGI("flyover: the host's %ls shows at (%.0f,%.0f,%.0f) -- sent", R::ClassNameOf(call.object).c_str(), at.X, at.Y,
            at.Z);
}

// HOST: each peeping UFO looks at the player nearest it.
void ServeUfos() {
    void* ufos[8];
    const int32_t n = ue_wrap::world_instances::Find(kMorning, ufos, 8);
    for (int32_t i = 0; i < n; ++i) {
        void* const ufo = ufos[i];
        ue_wrap::FVector at{};
        if (!ufo || !R::IsLive(ufo) || ReadBool(ufo, L"flyAway") == 1 || !ue_wrap::engine::TryGetActorLocation(ufo, at))
            continue;
        const uint8_t had = SP::ServedSlot(ufo), want = NearestPlayer(at);
        if (want == had) continue;
        if (want == 0) SP::Unserve(ufo);
        else if (!SP::Serve(ufo, want, SP::kPlayerCharacter)) continue;
        UE_LOGI("flyover: the peeping UFO looks at %s%u", want ? "slot " : "the host ", static_cast<unsigned>(want));
    }
}

// HOST: the mothership's alpha, for the copies.
void SendAlpha() {
    void* borgs[4];
    const int32_t n = ue_wrap::world_instances::Find(kBorg, borgs, 4);
    for (int32_t i = 0; i < n; ++i) {
        const float* const a = borgs[i] && R::IsLive(borgs[i]) ? FloatField(borgs[i], L"alpha") : nullptr;
        if (a) Send(kCueBorgAlpha, *a, 0.f, 0.f);
    }
}

bool Near(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz <= 1000.f * 1000.f;
}

void ShowMorning(const N::EventCuePayload& p) {
    float d = 3.4e38f;
    void* const ufo = NearestOf(kMorning, {p.x, p.y, p.z}, d);
    if (!ufo) {
        UE_LOGW("flyover: the host's peeping UFO shows, and this world has no copy of it");
        return;
    }
    WriteBool(ufo, L"fly", true);
    ue_wrap::engine::SetActorHiddenInGame(ufo, false);
    UE_LOGI("flyover: the host's peeping UFO shows here too, at (%.0f,%.0f,%.0f)", p.x, p.y, p.z);
}

// CLIENT: a saucer this machine loaded from the save it joined with ends where the host's copy of it stands, and where
// the host has none. One the host has with no copy beside it (the host loaded its own from its save, which the lane
// does not mirror) stays, inert, and a deploy shows it.
void EndLoadedTwins(uint64_t now) {
    if (!g_listDoneAt || now - g_listDoneAt > kTwinWindowMs) return;
    for (const wchar_t* k : kDropperKinds) {
        void* found[16];
        const int32_t n = ue_wrap::world_instances::Find(k, found, 16);
        for (int32_t i = 0; i < n; ++i) {
            void* const own = found[i];
            ue_wrap::FVector at{}, m{};
            if (!own || !R::IsLive(own) || coop::world_actor_sync::IsMirroredActor(own) ||
                !ue_wrap::engine::TryGetActorLocation(own, at))
                continue;
            bool hostHas = false;
            for (const ue_wrap::FVector& l : g_live) hostHas = hostHas || Near(l, at);
            if (!hostHas) {
                if (ue_wrap::engine::DestroyActor(own))
                    UE_LOGI("flyover: this machine's own %ls from its save ended -- the host has none there", k);
                continue;
            }
            for (int32_t j = 0; j < n; ++j) {
                if (j == i || !found[j] || !R::IsLive(found[j]) || !coop::world_actor_sync::IsMirroredActor(found[j]) ||
                    !ue_wrap::engine::TryGetActorLocation(found[j], m))
                    continue;
                if (!Near(m, at)) continue;
                if (ue_wrap::engine::DestroyActor(own))
                    UE_LOGI("flyover: this machine's own %ls from its save ended -- the host's copy stands there", k);
                break;
            }
        }
    }
}

void Deploy(const N::EventCuePayload& p) {
    float d = 3.4e38f;
    void* saucer = nullptr;
    for (const wchar_t* k : kDropperKinds)
        if (void* o = NearestOf(k, {p.x, p.y, p.z}, d)) saucer = o;
    if (!saucer) {
        UE_LOGW("flyover: the host's drop saucer deploys, and this world has no copy of it");
        return;
    }
    // As its box's run shows it: itself, its particles and its timeline; its tick on for the lights' blink.
    ue_wrap::engine::SetActorHiddenInGame(saucer, false);
    Run(Field(saucer, L"ParticleSystem"), L"Activate", 1);
    const bool played = Run(Field(saucer, L"Timeline_0"), L"PlayFromStart");
    ue_wrap::engine::SetActorTickEnabled(saucer, true);
    UE_LOGI("flyover: the host's %ls deploys here too, at (%.0f,%.0f,%.0f) (timeline=%d)",
            R::ClassNameOf(saucer).c_str(), p.x, p.y, p.z, played ? 1 : 0);
}

void TakeAlpha(float alpha) {
    void* borgs[4];
    const int32_t n = ue_wrap::world_instances::Find(kBorg, borgs, 4);
    for (int32_t i = 0; i < n; ++i)
        if (float* a = borgs[i] && R::IsLive(borgs[i]) ? FloatField(borgs[i], L"alpha") : nullptr) *a = alpha;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_morningBox && (g_morningBox = sg::WatchName(kRowMorningBox, kTagMorningBox, &OnClientRefusePre, &OnShowPost)))
        UE_LOGI("flyover: the peeping UFO's box is watched");
    if (!g_dropBegin && (g_dropBegin = sg::WatchName(kRowDropBegin, kTagDropBegin, &OnClientRefusePre, nullptr)))
        UE_LOGI("flyover: the drop saucer's begin play is watched");
    if (!g_dropBox && (g_dropBox = sg::WatchName(kRowDropBox, kTagDropBox, &OnClientRefusePre, &OnShowPost)))
        UE_LOGI("flyover: the drop saucer's box is watched");
    if (!g_dropForce && (g_dropForce = sg::WatchName(kRowDropForce, kTagDropForce, &OnClientRefusePre, &OnShowPost)))
        UE_LOGI("flyover: the drop saucer's deploy is watched");
    if (!g_dropDrop && (g_dropDrop = sg::WatchName(kRowDropDrop, kTagDropDrop, &OnClientRefusePre, nullptr)))
        UE_LOGI("flyover: the drop saucer's drop is watched");
    if (!g_dropEnd && (g_dropEnd = sg::WatchName(kRowDropEnd, kTagDropEnd, &OnClientRefusePre, nullptr)))
        UE_LOGI("flyover: the drop saucer's end is watched");
    const uint64_t now = ::GetTickCount64();
    if (s->role() == N::Role::Client && s->connected() && now >= g_nextServe) {
        g_nextServe = now + kAlphaMs;
        EndLoadedTwins(now);
    }
    if (s->role() != N::Role::Host || !s->connected()) return;
    if (now >= g_nextServe && SP::SeamsReady(SP::kPlayerCharacter)) {
        g_nextServe = now + kServeMs;
        ServeUfos();
    }
    if (now >= g_nextAlpha) {
        g_nextAlpha = now + kAlphaMs;
        SendAlpha();
    }
    // Ended ones leave the sent set.
    for (auto it = g_sent.begin(); it != g_sent.end();)
        it = R::IsLive(*it) ? std::next(it) : g_sent.erase(it);
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    auto send = [&](float x, float y, float z) {
        N::EventCuePayload p{};
        p.cueId = kCueDropperLive;
        p.x = x;
        p.y = y;
        p.z = z;
        s->SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
    };
    int n = 0;
    for (const wchar_t* k : kDropperKinds) {
        void* found[16];
        const int32_t m = ue_wrap::world_instances::Find(k, found, 16);
        ue_wrap::FVector at{};
        for (int32_t i = 0; i < m; ++i)
            if (found[i] && R::IsLive(found[i]) && ue_wrap::engine::TryGetActorLocation(found[i], at)) {
                send(at.X, at.Y, at.Z);
                ++n;
            }
    }
    send(0.f, 0.f, kListEndZ);
    UE_LOGI("flyover: slot %d's world is ready -- its list of the host's %d saucer(s) sent", slot, n);
}

void OnMirrorMaterialized(void* actor) {
    if (!actor) return;
    // The mothership flies its ships on its own tick, by the alpha the host sends.
    if (R::ClassNameOf(actor) == kBorg) ue_wrap::engine::SetActorTickEnabled(actor, true);
}

void OnMirrorEnding(void* actor) {
    if (!actor) return;
    // Its begin play set its event here, and only its own end at alpha 1 clears it; the host's end of it does here.
    if (R::ClassNameOf(actor) == kBorg && coop::creature_event_relay::RetractEntryOf(actor))
        UE_LOGI("flyover: CLIENT the host ended its mothership -- its event is ended with it");
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueMorningShow && p.cueId != kCueDropperDeploy && p.cueId != kCueBorgAlpha &&
        p.cueId != kCueDropperLive)
        return false;
    if (!IsRole(N::Role::Client)) return true;
    if (p.cueId == kCueDropperLive) {
        if (p.z > kListEndZ * 0.5f) {
            g_live.push_back({p.x, p.y, p.z});
        } else {
            g_listDoneAt = ::GetTickCount64();
            UE_LOGI("flyover: the host's %zu saucer(s) at the join -- this machine's own from its save are judged",
                    g_live.size());
        }
    }
    else if (p.cueId == kCueMorningShow) ShowMorning(p);
    else if (p.cueId == kCueDropperDeploy) Deploy(p);
    else TakeAlpha(p.x);
    return true;
}

void OnDisconnect() {
    g_sent.clear();
    g_live.clear();
    g_nextServe = g_nextAlpha = g_listDoneAt = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::flyover_sync
