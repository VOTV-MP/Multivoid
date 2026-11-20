// coop/world/flood_sync.cpp -- see coop/world/flood_sync.h.

#include "coop/world/flood_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::flood_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using Payload = N::FloodPayload;

constexpr const wchar_t* kPipeClass = L"burstWaterPipe_C";
constexpr const wchar_t* kVolumeClass = L"waterVolume_basementFlooder_C";
constexpr const wchar_t* kToolboxClass = L"prop_toolbox_C";
constexpr float kReachUU = 400.f;
constexpr float kLevelEps = 1.f;         // a level moved less than this (the volume is 500 high) is not sent
constexpr int kMaxIntentsPerSecond = 4;  // a sender's repairs past this in one second are dropped

UE_FACT_FN(kRowPipeDamage, "flood_sync", L"burstWaterPipe_C", L"addDamage", Declared);
UE_FACT_FN(kRowPipeFix, "flood_sync", L"burstWaterPipe_C", L"toolboxFix", Declared);
UE_FACT_FN(kRowPipeState, "flood_sync", L"burstWaterPipe_C", L"state", Declared);
UE_FACT_FN(kRowVolAddWater, "flood_sync", L"waterVolume_basementFlooder_C", L"addWater", Declared);
UE_FACT_FN(kRowVolSetLevel, "flood_sync", L"waterVolume_basementFlooder_C", L"setLevel", Declared);

enum Row { kPipeDamage, kPipeFix, kPipeState, kVolAddWater, kVolSetLevel, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowPipeDamage, &kRowPipeFix, &kRowPipeState, &kRowVolAddWater, &kRowVolSetLevel};
constexpr int kTagBase = 0x464C4400;   // 'FLD', then the row

std::atomic<N::Session*> g_session{nullptr};
uint32_t g_registered = 0;   // bit i: row i's watch registered
int g_applying = 0;          // inside our own apply: no watch reads it as a player's act

uint64_t Now() { return ::GetTickCount64(); }

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() ? s : nullptr;
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* const c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

std::wstring NameOf(void* o) {
    const std::wstring n = R::ToString(R::NameOf(o));
    return n.size() > sizeof(N::WireKey::data) ? std::wstring() : n;
}

void* ByName(const wchar_t* cls, const std::wstring& name) {
    if (name.empty()) return nullptr;
    for (void* o : All(cls))
        if (NameOf(o) == name) return o;
    return nullptr;
}

bool Damaged(void* pipe, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!pipe || !R::FindBoolProperty(R::ClassOf(pipe), L"damaged", off, mask)) return false;
    out = (static_cast<const uint8_t*>(pipe)[off] & mask) != 0;
    return true;
}

bool SetPipe(void* pipe, bool damaged) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"State"));
    return f.valid() && f.Set<bool>(L"damaged", damaged) && ue_wrap::Call(pipe, f);
}

bool Level(void* volume, float& out) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(volume), L"getLevel"));
    if (!f.valid() || !ue_wrap::Call(volume, f)) return false;
    out = f.Get<float>(L"level");
    return std::isfinite(out);
}

bool SetLevel(void* volume, float level) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(volume), L"setLevel"));
    if (!f.valid() || !f.Set<float>(L"alpha", level) || !ue_wrap::Call(volume, f)) return false;
    void* const upd = R::FindDispatchFunctionCached(R::ClassOf(volume), L"updateWaterData");
    if (upd) R::CallFunction(volume, upd, nullptr);
    return true;
}

void Fill(Payload& p, const std::wstring& name, uint8_t op, float level, bool damaged) {
    p = Payload{};
    N::WireKeyFromString(name, p.name);
    p.level = level;
    p.damaged = damaged ? 1 : 0;
    p.op = op;
}

// ---- HOST: the sends ----

std::unordered_map<std::wstring, bool> g_sentPipe;   // a pipe's bit as the clients last had it from a send to all
std::unordered_map<void*, float> g_sentLevel;        // a volume's level as the clients last had it from a send to all

// The drill's red arms (dev rows, never set in play): the host's changes are not sent / a client's repair is not judged /
// a joiner is sent nothing.
bool StateOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::flood_state_off);
    return off;
}
bool JudgeOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::flood_judge_off);
    return off;
}
bool JoinOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::flood_join_off);
    return off;
}

dev::Counts g_counts;

void Send(N::Session& s, const Payload& p, int slot) {
    if (slot >= 0) {
        s.SendReliableToSlot(slot, N::ReliableKind::FloodState, &p, sizeof(p));
        return;
    }
    if (StateOff()) return;
    s.SendReliable(N::ReliableKind::FloodState, &p, sizeof(p));
    if (p.op == N::kFloodOpPipe) ++g_counts.pipesSent;
    else {
        ++g_counts.levelsSent;
        g_counts.lastLevelSent = p.level;
    }
}

void SendPipe(N::Session& s, void* pipe, const std::wstring& name, int slot) {
    bool damaged = false;
    if (!Damaged(pipe, damaged)) return;
    Payload p;
    Fill(p, name, N::kFloodOpPipe, 0.f, damaged);
    Send(s, p, slot);
    if (slot < 0) g_sentPipe[name] = damaged;
}

void SendLevel(N::Session& s, void* volume, const std::wstring& name, int slot) {
    float level = 0.f;
    if (!Level(volume, level)) return;
    Payload p;
    Fill(p, name, N::kFloodOpLevel, level, false);
    Send(s, p, slot);
    if (slot < 0) g_sentLevel[volume] = level;
}

// A pipe's State has run, by any route (the hit's addDamage, the repair's toolboxFix, a save's loadData, our own
// apply of a client's repair): the event the clients are told on. addDamage calls State on every hit, so a bit the
// clients already have is not sent again.
void OnStateRan(const sg::Call& call) {
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Host) return;
    bool damaged = false;
    const std::wstring name = NameOf(call.object);
    if (name.empty() || !Damaged(call.object, damaged)) return;
    const auto it = g_sentPipe.find(name);
    if (it == g_sentPipe.end() ? !damaged : it->second == damaged) return;
    SendPipe(*s, call.object, name, -1);
    UE_LOGI("flood_sync: HOST pipe '%ls' %s -- sent", name.c_str(), damaged ? "burst" : "mended");
}

// The volume's level as getLevel reads it (its distance from loc_a), taken from memory: the actor's root location and the
// loc_a property, no UFunction call. A root with a parent has no world pose in memory, and the exact read goes through
// getLevel then.
bool LevelOf(void* volume, float& out) {
    static int32_t locA = -2;   // loc_a's offset; -1 when the class has none
    if (locA == -2) locA = R::FindPropertyOffset(R::ClassOf(volume), L"loc_a");
    ue_wrap::FVector at{};
    if (locA < 0 || !ue_wrap::engine::TryGetRootLocationRaw(volume, at)) return Level(volume, out);
    const ue_wrap::FVector a = *reinterpret_cast<const ue_wrap::FVector*>(static_cast<const char*>(volume) + locA);
    const float dx = at.X - a.X, dy = at.Y - a.Y, dz = at.Z - a.Z;
    out = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::isfinite(out);
}

// A volume's level was written by addWater (the burst pipes' ticks and the drain's tick, one call a frame each while they
// run) or by setLevel (a save's loadLevel): the event the clients are told on. A call that left the level within kLevelEps
// of what the clients have costs a memory read and a compare, and one that moved it a whole step is sent (about a unit a
// second).
void OnLevelRan(const sg::Call& call) {
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Host) return;
    float level = 0.f;
    if (!LevelOf(call.object, level)) return;
    const auto it = g_sentLevel.find(call.object);
    if (it == g_sentLevel.end() ? level < kLevelEps : std::fabs(it->second - level) < kLevelEps) return;
    const std::wstring name = NameOf(call.object);
    if (!name.empty()) SendLevel(*s, call.object, name, -1);
}

// ---- HOST: a client's repair ----

struct Rate {
    uint64_t second = 0;
    int n = 0;
};
Rate g_rate[N::kMaxPeers];

void Judge(N::Session& s, uint8_t slot, const Payload& p) {
    const std::wstring name = N::StringFromWireKey(p.name);
    if (!s.IsSlotWorldReady(slot)) return;
    void* const pipe = ByName(kPipeClass, name);
    if (!pipe) {
        UE_LOGI("flood_sync: slot %u's repair names no pipe ('%ls')", static_cast<unsigned>(slot), name.c_str());
        ++g_counts.judged;
        ++g_counts.refusedOther;
        g_counts.lastWhy = "no such pipe";
        return;
    }
    const char* why = nullptr;
    bool target = false;   // the refusal is the target check's: the sender's reach of the pipe
    bool damaged = false;
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) why = "dead";
    if (!why) {
        const coop::element::IntentSubject subject = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(pipe);
        if (!subject) {
            why = coop::element::OutcomeName(subject.outcome);
            target = true;
        }
    }
    if (!why && !coop::hand_item::HeldClassIs(slot, kToolboxClass)) why = "no toolbox in the hand";
    if (!why && (!Damaged(pipe, damaged) || !damaged)) why = "the pipe is whole";
    if (why && JudgeOff()) why = nullptr;   // the drill's red: every check passes
    if (why) {
        UE_LOGI("flood_sync: slot %u's repair of '%ls' refused: %s", static_cast<unsigned>(slot), name.c_str(), why);
        ++g_counts.judged;
        ++(target ? g_counts.refusedTarget : g_counts.refusedOther);
        g_counts.lastWhy = why;
        SendPipe(s, pipe, name, slot);
        return;
    }
    // The State call is the event the clients are told on (OnStateRan): the pipe's send is not made here.
    if (!SetPipe(pipe, false)) {
        UE_LOGW("flood_sync: slot %u's repair of '%ls': the pipe's State did not run", static_cast<unsigned>(slot), name.c_str());
        ++g_counts.judged;
        ++g_counts.refusedOther;
        g_counts.lastWhy = "State did not run";
        SendPipe(s, pipe, name, slot);
        return;
    }
    ++g_counts.judged;
    ++g_counts.mended;
    UE_LOGI("flood_sync: slot %u mended pipe '%ls' on the host", static_cast<unsigned>(slot), name.c_str());
}

// ---- CLIENT: the watches ----

void* g_fixPipe = nullptr;
bool g_fixWasDamaged = false;

sg::Verdict OnPre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount || g_applying || !coop::spawn_authority::IsActiveClientSession())
        return sg::Verdict::Run;
    // A pipe's burst and every write of the level (addWater from the pipes' and the drain's ticks, setLevel from a save's
    // loadLevel) are the host copy's: those calls only move the volume, so a client's level is the host's and nothing else.
    // Our own setLevel and State run with g_applying up and pass above.
    if (row != kPipeFix) return sg::Verdict::Cancel;
    g_fixPipe = call.object;
    g_fixWasDamaged = false;
    Damaged(call.object, g_fixWasDamaged);
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& call) {
    if (call.tag - kTagBase != kPipeFix || call.object != g_fixPipe) return;
    g_fixPipe = nullptr;
    bool damaged = true;
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Client || !g_fixWasDamaged || !Damaged(call.object, damaged) || damaged) return;
    const std::wstring name = NameOf(call.object);
    if (name.empty()) return;
    Payload p;
    Fill(p, name, N::kFloodOpFix, 0.f, false);
    s->SendReliable(N::ReliableKind::FloodIntent, &p, sizeof(p));
    UE_LOGI("flood_sync: this player mended pipe '%ls' -- the host mends it", name.c_str());
}

// A client's burst and level writes are cancelled and its repair watched; the host's State and level writes are watched for the send.
void Register() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_registered & (1u << i)) continue;
        const sg::PreFn pre = i == kPipeState ? nullptr : &OnPre;
        const sg::PostFn post = i == kPipeFix ? &OnPost : i == kPipeState ? &OnStateRan : i >= kVolAddWater ? &OnLevelRan : nullptr;
        if (sg::WatchName(*kRows[i], kTagBase + i, pre, post)) g_registered |= 1u << i;
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = Live();
    if (!s) return;
    if (g_registered != (1u << kRowCount) - 1) {
        static uint64_t nextTry = 0;
        if (Now() >= nextTry) {
            nextTry = Now() + 1000;
            Register();
            if (g_registered == (1u << kRowCount) - 1) UE_LOGI("flood_sync: the burst pipes' watches are live");
        }
    }
}

void OnState(const Payload& p) {
    const std::wstring name = N::StringFromWireKey(p.name);
    ++g_applying;
    if (p.op == N::kFloodOpPipe) {
        void* const pipe = ByName(kPipeClass, name);
        bool damaged = false;
        const bool want = p.damaged != 0;
        if (pipe && Damaged(pipe, damaged) && damaged != want && SetPipe(pipe, want))
            UE_LOGI("flood_sync: CLIENT pipe '%ls' %s, as the host's", name.c_str(), want ? "burst" : "mended");
    } else if (p.op == N::kFloodOpLevel && std::isfinite(p.level)) {
        if (void* const v = ByName(kVolumeClass, name)) SetLevel(v, p.level);
    }
    --g_applying;
}

void OnIntent(int senderSlot, const Payload& p) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host || senderSlot <= 0 || senderSlot >= N::kMaxPeers || p.op != N::kFloodOpFix) return;
    Rate& r = g_rate[senderSlot];
    const uint64_t sec = Now() / 1000;
    if (r.second != sec) r = Rate{sec, 0};
    if (++r.n > kMaxIntentsPerSecond) return;
    Judge(*s, static_cast<uint8_t>(senderSlot), p);
}

void SendJoin(int slot) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host || JoinOff()) return;
    for (void* pipe : All(kPipeClass)) {
        const std::wstring name = NameOf(pipe);
        if (!name.empty()) SendPipe(*s, pipe, name, slot);
    }
    for (void* v : All(kVolumeClass)) {
        const std::wstring name = NameOf(v);
        if (!name.empty()) SendLevel(*s, v, name, slot);
    }
}

void OnDisconnect() {
    g_sentPipe.clear();
    g_sentLevel.clear();
    for (Rate& r : g_rate) r = Rate{};
    g_fixPipe = nullptr;
}

namespace dev {

Counts Read() { return g_counts; }

float LevelEpsilon() { return kLevelEps; }

void* FirstPipe() {
    void* best = nullptr;
    std::wstring bestName;
    for (void* p : All(kPipeClass)) {
        const std::wstring n = R::ToString(R::NameOf(p));
        if (!best || n < bestName) {
            best = p;
            bestName = n;
        }
    }
    return best;
}

bool PipeBits(std::string& out) {
    std::vector<std::pair<std::wstring, char>> pipes;
    for (void* p : All(kPipeClass)) {
        bool damaged = false;
        if (!Damaged(p, damaged)) return false;
        pipes.emplace_back(R::ToString(R::NameOf(p)), damaged ? '1' : '0');
    }
    if (pipes.empty()) return false;
    std::sort(pipes.begin(), pipes.end());
    out.clear();
    for (const auto& p : pipes) out.push_back(p.second);
    return true;
}

bool LevelNow(float& out) {
    for (void* v : All(kVolumeClass))
        if (Level(v, out)) return true;
    return false;
}

}  // namespace dev

}  // namespace coop::flood_sync
