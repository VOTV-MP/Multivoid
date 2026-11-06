// coop/interactables/atv_host_sync.cpp -- see coop/interactables/atv_host_sync.h.

#include "coop/interactables/atv_host_sync.h"

#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::atv_host_sync {
namespace {

namespace A  = ue_wrap::atv;
namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AtvHostPayload;
namespace op = coop::net::atv_host_op;

constexpr float kMax = 100.f;
constexpr float kGainMin = 0.5f;       // a client's copy rose by this much: a refuel or a repair
constexpr float kSendStep = 0.5f;      // the host sends when fuel or health moved by this
constexpr float kReachCm = 800.f;      // a refuel's, repair's or module's sender from its ATV, on the host
constexpr size_t kQueueCap = 64;
constexpr auto kKeepalive = std::chrono::seconds(2);
constexpr auto kHold = std::chrono::milliseconds(1500);

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};
uint32_t g_world = 0;
int g_applying = 0;   // our own writes run inside this
uint32_t g_hornsPlayed = 0;

struct ApplyScope {
    ApplyScope() { ++g_applying; }
    ~ApplyScope() { --g_applying; }
};

// ---- the ATV's fields and bodies ----

void* g_cls = nullptr;
int32_t g_offFuel = -1, g_offHealth = -1, g_offEmpty = -1, g_offBroken = -1, g_offModules = -1, g_offLights = -1,
        g_offBrake = -1, g_offTurbo = -1, g_offSpeed = -1, g_offSpeedTurbo = -1, g_offInputAlt = -1, g_offFly = -1;

bool Resolve() {
    if (g_cls) return true;
    void* cls = R::FindClass(L"ATV_C");
    if (!cls) return false;
    struct { int32_t* off; const wchar_t* name; } rows[] = {
        {&g_offFuel, L"fuel"}, {&g_offHealth, L"health"}, {&g_offEmpty, L"Empty"}, {&g_offBroken, L"brokenn"},
        {&g_offModules, L"Modules"}, {&g_offLights, L"lights"}, {&g_offBrake, L"Brake"}, {&g_offTurbo, L"turbo"},
        {&g_offSpeed, L"Speed"}, {&g_offSpeedTurbo, L"speed_turbo"}, {&g_offInputAlt, L"input_alt"},
        {&g_offFly, L"Fly"},
    };
    for (auto& r : rows) {
        *r.off = R::FindPropertyOffset(cls, r.name);
        if (*r.off < 0) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                UE_LOGE("atv_host: ATV_C has no `%ls` -- the ATV's host state is not carried", r.name);
            }
            return false;
        }
    }
    g_cls = cls;
    return true;
}

template <class T>
T Get(void* a, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(a) + off, sizeof(T));
    return v;
}

template <class T>
void Put(void* a, int32_t off, const T& v) {
    std::memcpy(static_cast<uint8_t*>(a) + off, &v, sizeof(T));
}

bool GetBool(void* a, int32_t off) { return static_cast<const uint8_t*>(a)[off] != 0; }
void PutBool(void* a, int32_t off, bool v) { static_cast<uint8_t*>(a)[off] = v ? 1 : 0; }

bool Call(void* a, const wchar_t* fn) { return ue_wrap::component_calls::CallParamlessNamed(a, fn); }

// An input event of the ATV (its key a zeroed struct): the horn's press and release.
bool CallInput(void* a, const wchar_t* fn) {
    void* f = R::FindFunction(R::ClassOf(a), fn);
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    return pf.valid() && ue_wrap::Call(a, pf);
}

constexpr const wchar_t* kHornPress = L"InpActEvt_atv_honk_K2Node_InputActionEvent_14";
constexpr const wchar_t* kHornRelease = L"InpActEvt_atv_honk_K2Node_InputActionEvent_15";

std::vector<uint8_t> ReadModules(void* a) {
    const SR::Arr arr = SR::ReadArr(a, g_offModules);
    std::vector<uint8_t> out;
    for (int32_t i = 0; i < arr.num && i < 64 && arr.data; ++i) out.push_back(arr.data[i]);
    return out;
}

// A new Modules array in engine memory (the old buffer is left to the engine's own lifetime, as save_record does), then
// the ATV's own updUpgrades, which fits or takes off every part to match.
void WriteModules(void* a, const std::vector<uint8_t>& mods) {
    void* buf = mods.empty() ? nullptr : SR::AllocZeroed(mods.size(), 1);
    if (!mods.empty() && !buf) return;
    if (buf) std::memcpy(buf, mods.data(), mods.size());
    SR::WriteArrHeader(a, g_offModules, buf, static_cast<int32_t>(mods.size()));
    Call(a, L"updUpgrades");
}

uint8_t ReadFlags(void* a) {
    uint8_t f = 0;
    if (GetBool(a, g_offLights)) f |= coop::net::kAtvHostLights;
    if (GetBool(a, g_offBrake)) f |= coop::net::kAtvHostBrake;
    if (GetBool(a, g_offTurbo)) f |= coop::net::kAtvHostTurbo;
    if (GetBool(a, g_offEmpty)) f |= coop::net::kAtvHostEmpty;
    if (GetBool(a, g_offBroken)) f |= coop::net::kAtvHostBroken;
    return f;
}

constexpr uint8_t kControls = coop::net::kAtvHostLights | coop::net::kAtvHostBrake | coop::net::kAtvHostTurbo;

// The ATV's own bodies for the three controls: `upd lights`, setBrake, and unturbo or the turbo key's speed.
void ApplyControls(void* a, uint8_t want) {
    const uint8_t have = ReadFlags(a);
    if ((want ^ have) & coop::net::kAtvHostLights) {
        PutBool(a, g_offLights, (want & coop::net::kAtvHostLights) != 0);
        Call(a, L"Upd Lights");
    }
    if ((want ^ have) & coop::net::kAtvHostBrake) {
        PutBool(a, g_offBrake, (want & coop::net::kAtvHostBrake) != 0);
        Call(a, L"setBrake");
    }
    if ((want ^ have) & coop::net::kAtvHostTurbo) {
        if (want & coop::net::kAtvHostTurbo) {
            PutBool(a, g_offTurbo, true);
            Put<float>(a, g_offSpeed, Get<float>(a, g_offSpeedTurbo));
        } else {
            Call(a, L"unturbo");
        }
    }
}

// The ATV's own horn on this copy; the press and release also write input_alt and the Fly module's lift, which a copy
// nobody drives keeps as it was.
void PlayHorn(void* a, bool press) {
    const bool alt = GetBool(a, g_offInputAlt), fly = GetBool(a, g_offFly);
    CallInput(a, press ? kHornPress : kHornRelease);
    PutBool(a, g_offInputAlt, alt);
    PutBool(a, g_offFly, fly);
}

std::string ModulesText(const std::vector<uint8_t>& m) {
    std::string s;
    for (uint8_t b : m) s += (s.empty() ? "" : ",") + std::to_string(b);
    return "[" + s + "]";
}

// ---- the ATVs ----

struct Indexed {
    std::wstring key;
    void* actor;
};

void Collect(void* ctx, const std::wstring& key, void* actor) {
    static_cast<std::vector<Indexed>*>(ctx)->push_back({key, actor});
}

std::vector<Indexed> Atvs() {
    std::vector<Indexed> out;
    coop::atv_sync::ForEachIndexed(&Collect, &out);
    return out;
}

void* Find(const std::wstring& key) {
    coop::atv_sync::TableView t;
    return coop::atv_sync::ReadTable(key, t) ? t.actor : nullptr;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

bool Finite(float f) { return std::isfinite(f); }

// ---- HOST ----

struct Sent {
    float fuel = 0.f, health = 0.f;
    uint8_t flags = 0;
    std::vector<uint8_t> modules;
    uint32_t gen = 0;
    Clock::time_point at{};
    bool valid = false, pending = false;
};
std::unordered_map<std::wstring, Sent> g_sent;
std::unordered_map<std::wstring, uint32_t> g_gen;   // the gains the host applied, per ATV

struct Request {
    int slot = 0;
    Payload p{};
};
std::vector<Request> g_requests;
uint32_t g_hostGains = 0, g_hostModules = 0, g_hostControls = 0, g_hostRefused = 0;

bool InReach(int slot, void* atv) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{}, by{};
    if (!body || !ue_wrap::engine::TryGetActorLocation(atv, at) || !ue_wrap::engine::TryGetActorLocation(body, by))
        return false;
    const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
    return dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm;
}

// A fit the host would not make is given back: the module's prop at the ATV, as the take-off hands one over.
void Refund(void* atv, uint8_t module) {
    void* cls = ue_wrap::phys_mods::ClassForByte(module);
    ue_wrap::FVector at{};
    if (!cls || !ue_wrap::engine::TryGetActorLocation(atv, at)) {
        UE_LOGW("atv_host: no prop class for module %u -- the refused fit is not refunded", static_cast<unsigned>(module));
        return;
    }
    ue_wrap::engine::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
}

void HostServe(coop::net::Session& s) {
    std::vector<Request> requests;
    requests.swap(g_requests);
    for (const Request& r : requests) {
        const std::wstring key = coop::net::StringFromWireKey(r.p.key);
        if (!s.IsSlotWorldReady(r.slot)) continue;
        void* a = Find(key);
        if (!a) continue;
        coop::atv_sync::TableView t;
        coop::atv_sync::ReadTable(key, t);
        g_sent[key].pending = true;   // the answer, whatever it is
        const bool seated = t.occupantSlot == static_cast<uint8_t>(r.slot);
        const bool driverOp = r.p.op == op::kControls || r.p.op == op::kHornPress || r.p.op == op::kHornRelease;
        if (driverOp ? !seated : (!seated && !InReach(r.slot, a))) {
            ++g_hostRefused;
            UE_LOGW("atv_host: slot %d's op %u on '%ls' refused -- %s", r.slot, static_cast<unsigned>(r.p.op),
                    key.c_str(), driverOp ? "not its seat" : "out of reach");
            if (r.p.op == op::kModuleAdd) Refund(a, r.p.module);
            continue;
        }
        ApplyScope applying;
        switch (r.p.op) {
        case op::kFuelGain: {
            const float from = Get<float>(a, g_offFuel), to = std::fmin(from + r.p.amount, kMax);
            Put<float>(a, g_offFuel, to);
            if (to > 0.f) PutBool(a, g_offEmpty, false);
            ++g_gen[key];
            ++g_hostGains;
            UE_LOGI("atv_host: slot %d refuelled '%ls' on the host: %.1f -> %.1f", r.slot, key.c_str(), from, to);
            break;
        }
        case op::kHealthGain: {
            const float from = Get<float>(a, g_offHealth), to = std::fmin(from + r.p.amount, kMax);
            Put<float>(a, g_offHealth, to);
            if (to > 0.f) PutBool(a, g_offBroken, false);
            Call(a, L"updHealth");
            ++g_gen[key];
            ++g_hostGains;
            UE_LOGI("atv_host: slot %d repaired '%ls' on the host: %.1f -> %.1f", r.slot, key.c_str(), from, to);
            break;
        }
        case op::kModuleAdd: {
            std::vector<uint8_t> mods = ReadModules(a);
            if (std::find(mods.begin(), mods.end(), r.p.module) != mods.end()) {
                ++g_hostRefused;
                UE_LOGW("atv_host: slot %d fitted module %u on '%ls', which the host has -- refunded", r.slot,
                        static_cast<unsigned>(r.p.module), key.c_str());
                Refund(a, r.p.module);
                break;
            }
            mods.push_back(r.p.module);
            WriteModules(a, mods);
            ++g_hostModules;
            UE_LOGI("atv_host: slot %d fitted module %u on '%ls' on the host -> %hs", r.slot,
                    static_cast<unsigned>(r.p.module), key.c_str(), ModulesText(ReadModules(a)).c_str());
            break;
        }
        case op::kModuleRemove: {
            std::vector<uint8_t> mods = ReadModules(a);
            auto it = std::find(mods.begin(), mods.end(), r.p.module);
            if (it == mods.end()) {
                ++g_hostRefused;
                UE_LOGW("atv_host: slot %d took module %u off '%ls', which the host does not have -- refused", r.slot,
                        static_cast<unsigned>(r.p.module), key.c_str());
                break;
            }
            mods.erase(it);
            WriteModules(a, mods);
            ++g_hostModules;
            UE_LOGI("atv_host: slot %d took module %u off '%ls' on the host -> %hs", r.slot,
                    static_cast<unsigned>(r.p.module), key.c_str(), ModulesText(ReadModules(a)).c_str());
            break;
        }
        case op::kControls:
            ApplyControls(a, r.p.flags & kControls);
            ++g_hostControls;
            UE_LOGI("atv_host: slot %d's controls on '%ls': lights=%d brake=%d turbo=%d", r.slot, key.c_str(),
                    (r.p.flags & coop::net::kAtvHostLights) ? 1 : 0, (r.p.flags & coop::net::kAtvHostBrake) ? 1 : 0,
                    (r.p.flags & coop::net::kAtvHostTurbo) ? 1 : 0);
            break;
        case op::kHornPress:
        case op::kHornRelease: {
            const bool press = r.p.op == op::kHornPress;
            PlayHorn(a, press);
            if (press) ++g_hornsPlayed;
            Payload out{};
            out.key = r.p.key;
            out.op = press ? op::kStateHornPress : op::kStateHornRelease;
            s.SendReliable(coop::net::ReliableKind::AtvHostState, &out, sizeof(out), r.slot);
            if (press) UE_LOGI("atv_host: slot %d sounded the horn of '%ls' -- played here and sent on", r.slot, key.c_str());
            break;
        }
        default:
            break;
        }
    }
}

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    const std::vector<Indexed> atvs = Atvs();
    for (const Indexed& x : atvs) {
        void* a = x.actor;
        const float fuel = Get<float>(a, g_offFuel), health = Get<float>(a, g_offHealth);
        if (!Finite(fuel) || !Finite(health)) continue;
        const uint8_t flags = ReadFlags(a);
        const std::vector<uint8_t> mods = ReadModules(a);
        const uint32_t gen = g_gen[x.key];
        Sent& sent = g_sent[x.key];
        const bool due = sent.pending || !sent.valid || flags != sent.flags || mods != sent.modules || gen != sent.gen ||
                         std::fabs(fuel - sent.fuel) >= kSendStep || std::fabs(health - sent.health) >= kSendStep ||
                         now - sent.at >= kKeepalive;
        if (!due) continue;
        Payload p{};
        coop::net::WireKeyFromString(x.key, p.key);
        p.op = op::kState;
        p.fuel = fuel;
        p.health = health;
        p.gen = gen;
        p.flags = flags;
        p.moduleCount = static_cast<uint8_t>(std::min<size_t>(mods.size(), sizeof(p.modules)));
        std::memcpy(p.modules, mods.data(), p.moduleCount);
        if (!s.SendReliable(coop::net::ReliableKind::AtvHostState, &p, sizeof(p))) continue;
        sent.fuel = fuel;
        sent.health = health;
        sent.flags = flags;
        sent.modules = mods;
        sent.gen = gen;
        sent.at = now;
        sent.valid = true;
        sent.pending = false;
    }
}

// ---- CLIENT ----

struct Copy {
    float fuel = 0.f, health = 0.f;      // the copy's values at the last write or reading
    bool vitalsKnown = false;
    std::vector<uint8_t> hostModules;     // the host's last modules
    bool modulesKnown = false;
    uint8_t controls = 0;                 // the controls last sent or taken from the host
    bool controlsKnown = false;
    bool inputAlt = false;
    uint32_t gen = 0;
    Clock::time_point holdVitals{}, holdModules{}, holdControls{};
};
std::unordered_map<std::wstring, Copy> g_copies;

struct Shown {
    Payload p{};
    Clock::time_point at{};
};
std::unordered_map<std::wstring, Shown> g_shown;   // the host's newest state per ATV, until it is written
std::vector<Payload> g_outbox;
uint32_t g_clientGains = 0, g_clientModules = 0, g_clientControls = 0, g_clientDropped = 0;

void Queue(const Payload& p) {
    if (g_outbox.size() >= kQueueCap) {
        ++g_clientDropped;
        return;
    }
    g_outbox.push_back(p);
}

Payload MakeRequest(const std::wstring& key, uint8_t opcode) {
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = opcode;
    return p;
}

// A rise in the copy's fuel or health past the last value it held is this player's refuel or repair.
void DetectGains(void* a, const std::wstring& key, Copy& c, Clock::time_point now) {
    const float fuel = Get<float>(a, g_offFuel), health = Get<float>(a, g_offHealth);
    if (!Finite(fuel) || !Finite(health)) return;
    if (c.vitalsKnown) {
        if (fuel > c.fuel + kGainMin) {
            Payload p = MakeRequest(key, op::kFuelGain);
            p.amount = std::fmin(fuel - c.fuel, kMax);
            Queue(p);
            c.holdVitals = now + kHold;
            ++g_clientGains;
            UE_LOGI("atv_host: this player refuelled '%ls' by %.1f -- the host refuels it", key.c_str(), p.amount);
        }
        if (health > c.health + kGainMin) {
            Payload p = MakeRequest(key, op::kHealthGain);
            p.amount = std::fmin(health - c.health, kMax + 1.f);
            Queue(p);
            c.holdVitals = now + kHold;
            ++g_clientGains;
            UE_LOGI("atv_host: this player repaired '%ls' by %.1f -- the host repairs it", key.c_str(), p.amount);
        }
    }
    c.fuel = fuel;
    c.health = health;
    c.vitalsKnown = true;
}

// A change to the copy's Modules that the host's state did not make is this player's fit or take-off.
void DetectModules(void* a, const std::wstring& key, Copy& c, Clock::time_point now) {
    if (!c.modulesKnown) return;
    const std::vector<uint8_t> mods = ReadModules(a);
    if (mods == c.hostModules) return;
    for (uint8_t m : mods)
        if (std::find(c.hostModules.begin(), c.hostModules.end(), m) == c.hostModules.end()) {
            Payload p = MakeRequest(key, op::kModuleAdd);
            p.module = m;
            Queue(p);
            ++g_clientModules;
            UE_LOGI("atv_host: this player fitted module %u on '%ls' -- the host fits it", static_cast<unsigned>(m),
                    key.c_str());
        }
    for (uint8_t m : c.hostModules)
        if (std::find(mods.begin(), mods.end(), m) == mods.end()) {
            Payload p = MakeRequest(key, op::kModuleRemove);
            p.module = m;
            Queue(p);
            ++g_clientModules;
            UE_LOGI("atv_host: this player took module %u off '%ls' -- the host takes it off", static_cast<unsigned>(m),
                    key.c_str());
        }
    c.hostModules = mods;   // sent; the host's answer replaces it
    c.holdModules = now + kHold;
}

// The seated driver's own controls (its horn is read every frame, in Tick).
void DetectDriver(void* a, const std::wstring& key, Copy& c, Clock::time_point now) {
    const uint8_t controls = ReadFlags(a) & kControls;
    if (c.controlsKnown && controls != c.controls) {
        Payload p = MakeRequest(key, op::kControls);
        p.flags = controls;
        Queue(p);
        c.holdControls = now + kHold;
        ++g_clientControls;
    }
    c.controls = controls;
    c.controlsKnown = true;
}

void ApplyState(void* a, const std::wstring& key, Copy& c, const Payload& p, Clock::time_point now, bool syncer) {
    ApplyScope applying;
    // Fuel and health: a copy this client does not sync takes them; the one it drives takes a gain the host made (its
    // counter moved) or a value lower than its own.
    if (now >= c.holdVitals) {
        const float fuel = Get<float>(a, g_offFuel), health = Get<float>(a, g_offHealth);
        const bool gained = p.gen != c.gen;
        const bool take = !syncer || gained;
        const float toFuel = take ? p.fuel : std::fmin(fuel, p.fuel);
        const float toHealth = take ? p.health : std::fmin(health, p.health);
        if (toFuel != fuel) Put<float>(a, g_offFuel, toFuel);
        if (toHealth != health) {
            Put<float>(a, g_offHealth, toHealth);
            Call(a, L"updHealth");
        }
        PutBool(a, g_offEmpty, (p.flags & coop::net::kAtvHostEmpty) != 0);
        PutBool(a, g_offBroken, (p.flags & coop::net::kAtvHostBroken) != 0);
        c.fuel = toFuel;
        c.health = toHealth;
        c.vitalsKnown = true;
        c.gen = p.gen;
    }
    if (now >= c.holdModules) {
        std::vector<uint8_t> mods(p.modules, p.modules + std::min<size_t>(p.moduleCount, sizeof(p.modules)));
        if (mods != ReadModules(a)) {
            WriteModules(a, mods);
            UE_LOGI("atv_host: the host's '%ls' has modules %hs here", key.c_str(), ModulesText(mods).c_str());
        }
        c.hostModules = mods;
        c.modulesKnown = true;
    }
    if (now >= c.holdControls) {
        ApplyControls(a, p.flags & kControls);
        c.controls = p.flags & kControls;
        c.controlsKnown = true;
    }
}

void ClientPoll(coop::net::Session& s, Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    for (const Indexed& x : Atvs()) {
        Copy& c = g_copies[x.key];
        DetectGains(x.actor, x.key, c, now);
        DetectModules(x.actor, x.key, c, now);
        coop::atv_sync::TableView t;
        if (coop::atv_sync::ReadTable(x.key, t) && t.occupantSlot == self) DetectDriver(x.actor, x.key, c, now);
        else c.controlsKnown = false;
    }
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s.SendReliable(coop::net::ReliableKind::AtvRequest, &*it, sizeof(*it))) break;
        it = g_outbox.erase(it);
    }
    for (auto it = g_shown.begin(); it != g_shown.end();) {
        void* a = Find(it->first);
        if (!a) {
            it = now - it->second.at > std::chrono::seconds(10) ? g_shown.erase(it) : std::next(it);
            continue;
        }
        coop::atv_sync::TableView t;
        coop::atv_sync::ReadTable(it->first, t);
        Copy& c = g_copies[it->first];
        DetectGains(a, it->first, c, now);   // a refuel since the poll above stands before the host's values
        ApplyState(a, it->first, c, it->second.p, now, t.syncerSlot == self);
        it = g_shown.erase(it);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    coop::net::Session* s = Live();
    if (!s || !Resolve()) return;
    const Clock::time_point now = Clock::now();
    // The local driver's horn is read every frame, a tap being shorter than the poll: a client's goes to the host, the
    // host's own straight to every client.
    const bool host = s->role() == coop::net::Role::Host;
    if (host || coop::net_pump::HasAnnouncedWorldReady()) {
        const uint8_t self = host ? 0 : coop::players::Registry::Get().LocalPeerId();
        if (void* a = coop::atv_sync::ActorSeatedBy(self)) {
            for (const Indexed& x : Atvs()) {
                if (x.actor != a) continue;
                Copy& c = g_copies[x.key];
                const bool alt = GetBool(a, g_offInputAlt);
                if (alt != c.inputAlt && !g_applying) {
                    if (host) {
                        Payload out{};
                        coop::net::WireKeyFromString(x.key, out.key);
                        out.op = alt ? op::kStateHornPress : op::kStateHornRelease;
                        s->SendReliable(coop::net::ReliableKind::AtvHostState, &out, sizeof(out));
                    } else {
                        Queue(MakeRequest(x.key, alt ? op::kHornPress : op::kHornRelease));
                    }
                    c.inputAlt = alt;
                }
            }
        }
    }
    if (!g_poll.Due(now)) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        g_sent.clear();
        g_copies.clear();
        g_shown.clear();
        g_outbox.clear();
        g_requests.clear();
    }
    if (s->role() == coop::net::Role::Host) {
        HostServe(*s);
        HostPoll(*s, now);
    } else if (s->role() == coop::net::Role::Client) {
        ClientPoll(*s, now);
    }
}

void OnState(const coop::net::AtvHostPayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty() || !Resolve()) return;
    if (p.op == op::kStateHornPress || p.op == op::kStateHornRelease) {
        if (void* a = Find(key)) {
            ApplyScope applying;
            PlayHorn(a, p.op == op::kStateHornPress);
            if (p.op == op::kStateHornPress) ++g_hornsPlayed;
        }
        return;
    }
    if (p.op != op::kState || !Finite(p.fuel) || !Finite(p.health) || p.moduleCount > sizeof(p.modules)) return;
    Shown& sh = g_shown[key];
    sh.p = p;
    sh.at = Clock::now();
}

void OnRequest(int senderSlot, const coop::net::AtvHostPayload& p) {
    if (senderSlot <= 0 || senderSlot >= static_cast<int>(coop::net::kMaxPeers) || g_requests.size() >= kQueueCap) return;
    switch (p.op) {
    case op::kFuelGain:
    case op::kHealthGain:
        if (!Finite(p.amount) || p.amount <= 0.f || p.amount > kMax + 1.f) return;
        break;
    case op::kModuleAdd:
    case op::kModuleRemove:
        if (p.module >= 64) return;
        break;
    case op::kControls:
    case op::kHornPress:
    case op::kHornRelease:
        break;
    default:
        return;
    }
    g_requests.push_back({senderSlot, p});
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    if (g_hostGains || g_hostModules || g_hostControls || g_hostRefused || g_clientGains || g_clientModules ||
        g_clientControls || g_clientDropped)
        UE_LOGI("atv_host: session end -- host gains=%u modules=%u controls=%u refused=%u; client gains=%u modules=%u "
                "controls=%u dropped=%u", g_hostGains, g_hostModules, g_hostControls, g_hostRefused, g_clientGains,
                g_clientModules, g_clientControls, g_clientDropped);
    g_hostGains = g_hostModules = g_hostControls = g_hostRefused = 0;
    g_clientGains = g_clientModules = g_clientControls = g_clientDropped = 0;
    g_sent.clear();
    g_gen.clear();
    g_copies.clear();
    g_shown.clear();
    g_outbox.clear();
    g_requests.clear();
    g_session.store(nullptr);
}

void FilterVitals(void* actor, const std::wstring& key, bool hostTakingClientBlock, float& fuel, float& health) {
    if (!actor || !Resolve()) return;
    const float curFuel = Get<float>(actor, g_offFuel), curHealth = Get<float>(actor, g_offHealth);
    if (!Finite(curFuel) || !Finite(curHealth)) return;
    if (hostTakingClientBlock) {
        // A driver's block lowers them only; a gain is the host's to apply.
        fuel = std::fmin(fuel, curFuel);
        health = std::fmin(health, curHealth);
        return;
    }
    // A client taking the host's block: its own refuel or repair goes to the host before the block writes over it.
    Copy& c = g_copies[key];
    const Clock::time_point now = Clock::now();
    DetectGains(actor, key, c, now);
    if (now < c.holdVitals) {
        fuel = curFuel;
        health = curHealth;
        return;
    }
    c.fuel = fuel;
    c.health = health;
}

uint32_t HornsPlayed() { return g_hornsPlayed; }

}  // namespace coop::atv_host_sync
