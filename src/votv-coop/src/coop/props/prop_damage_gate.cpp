// coop/props/prop_damage_gate.cpp -- see coop/props/prop_damage_gate.h.

#include "coop/props/prop_damage_gate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::props::prop_damage_gate {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// comp_physicsImpact_C's own two damage entries: fireDamage ends in the owner's destroy without passing impactDamage.
UE_FACT_FN(kRowImpact, "prop_damage_gate", L"comp_physicsImpact_C", L"impactDamage", Declared);
UE_FACT_FN(kRowFire, "prop_damage_gate", L"comp_physicsImpact_C", L"fireDamage", Declared);

enum Row { kImpact, kFire };
constexpr int kRows = 2;
constexpr int kTag[kRows] = {0x50444731, 0x50444732};  // 'PDG1', 'PDG2'
const ue_wrap::facts::Function* const kRowOf[kRows] = {&kRowImpact, &kRowFire};
const char* const kVerb[kRows] = {"impactDamage", "fireDamage"};

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// Never set in a shipped run: the gate does nothing (the red of the drill's break and fire arms).
bool Off() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::prop_damage_gate_off);
    return s;
}

// Never set in a shipped run: a client's own authorship no longer spares a prop (the red of the held-break arm).
bool Strict() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::prop_damage_gate_strict);
    return s;
}

unsigned long long                g_cancelled[kRows] = {};
unsigned long long                g_held[kRows] = {};   // lethal calls cancelled while a grant is awaited
unsigned long long                g_replayed = 0;
ue_wrap::ObjKeyedCache<bool, 24>  g_said[kRows];   // the owner classes whose first cancelled call has been said
ue_wrap::ObjKeyedCache<bool, 24>  g_heldSaid[kRows];

R::ParamOffset g_damageOff[kRows] = {R::ParamOffset{L"damage"}, R::ParamOffset{L"damage"}};
R::ParamOffset g_impactOff{L"impact"};
R::ParamOffset g_hitOff{L"hit"};
R::ParamOffset g_actorOff{L"actor"};

// A parameter's storage in the call: the caller's, for an out or reference parameter, else the frame's own slot.
const uint8_t* ParamBytes(const sg::Call& c, int32_t offset) {
    if (offset < 0 || !c.locals) return nullptr;
    const uint8_t* out = sg::OutParamPtr(c, offset);
    return out ? out : c.locals + offset;
}

// Where the component keeps its health and the resistance its damage is divided by (damageData's inner member).
struct CompOffsets {
    int32_t health = -1;
    int32_t damageData = -1;
    int32_t resistance = -1;
    bool    resolved = false;
};

const CompOffsets& OffsetsOf(void* comp) {
    static CompOffsets o;
    if (o.resolved) return o;
    void* cls = R::ClassOf(comp);
    if (!cls) return o;
    o.health = R::FindPropertyOffset(cls, L"health");
    o.damageData = R::FindPropertyOffset(cls, L"damageData");
    void* st = R::PropertyInnerStruct(cls, L"damageData");
    o.resistance = st ? R::FindPropertyOffsetByPrefix(st, L"damageResistance_") : -1;
    o.resolved = true;
    if (o.health < 0 || o.damageData < 0 || o.resistance < 0)
        UE_LOGE("prop_damage_gate: the component's health (%d) or damageData.damageResistance (%d+%d) did not resolve -- "
                "a lethal call on a prop awaiting its grant runs", o.health, o.damageData, o.resistance);
    return o;
}

// Whether this call would bring the component's health to zero or below, by the component's own arithmetic: an impact
// is gated on damage / resistance > 1 and takes that quotient from the health; a fire takes the damage itself. A
// resistance of 0 is lethal, as the division makes it. False when the figures cannot be read.
bool Lethal(Row row, const sg::Call& c) {
    const CompOffsets& o = OffsetsOf(c.object);
    if (o.health < 0 || o.damageData < 0 || o.resistance < 0) return false;
    float damage = 0.f;
    const uint8_t* d = ParamBytes(c, g_damageOff[row].Of(c.function));
    if (!d) return false;
    std::memcpy(&damage, d, sizeof(damage));
    const uint8_t* base = static_cast<const uint8_t*>(c.object);
    float health = 0.f, resistance = 0.f;
    std::memcpy(&health, base + o.health, sizeof(health));
    std::memcpy(&resistance, base + o.damageData + o.resistance, sizeof(resistance));
    if (row == kFire) return health - damage <= 0.f;
    const float taken = damage / resistance;
    return taken > 1.f && health - taken <= 0.f;
}

// This client authors the prop only by a sequence its swing or throw minted and the host has not granted yet.
bool AuthoredByPendingSeqOnly(void* prop) {
    if (!coop::prop_throw::IsPending(prop)) return false;   // the cheap test: a scan of at most eight records
    void* me = coop::players::Registry::Get().Local();
    if (me && ue_wrap::engine::IsMainPlayerGrabbing(me, prop)) return false;
    return !coop::prop_syncer::AuthorsActorHere(prop);
}

// The impactDamage call as values, for the record to keep.
void CaptureBreak(const sg::Call& c, coop::prop_throw::DeferredBreak& b) {
    b.comp.Set(c.object);
    if (const uint8_t* d = ParamBytes(c, g_damageOff[kImpact].Of(c.function))) std::memcpy(&b.damage, d, sizeof(b.damage));
    if (const uint8_t* i = ParamBytes(c, g_impactOff.Of(c.function))) std::memcpy(b.impact, i, sizeof(b.impact));
    void* causer = nullptr;
    if (const uint8_t* a = ParamBytes(c, g_actorOff.Of(c.function))) std::memcpy(&causer, a, sizeof(causer));
    if (causer && R::IsLive(causer)) b.causer.Set(causer);
    const int32_t hitSize = R::FindParamSize(c.function, L"hit");
    const uint8_t* h = ParamBytes(c, g_hitOff.Of(c.function));
    if (h && hitSize > 0 && hitSize <= coop::prop_throw::kMaxHitBytes) {
        std::memcpy(b.hit, h, static_cast<size_t>(hitSize));
        b.hitSize = hitSize;
    }
}

// A lethal call on a prop whose grant is awaited: cancelled. An impact is held for the grant, a fire is dropped.
sg::Verdict HoldLethal(Row row, const sg::Call& c, void* owner) {
    ++g_held[row];
    void* const cls = R::ClassOf(owner);
    if (!g_heldSaid[row].Find(cls)) {
        g_heldSaid[row].Insert(cls, true);
        UE_LOGI("prop_damage_gate: a lethal %s on a prop awaiting its grant is %s (cls=%ls)", kVerb[row],
                row == kImpact ? "held for it" : "dropped", R::ClassNameOf(owner).c_str());
    }
    if (row == kImpact) {
        coop::prop_throw::DeferredBreak b;
        CaptureBreak(c, b);
        coop::prop_throw::HoldBreak(owner, b);
    }
    return sg::Verdict::Cancel;
}

sg::Verdict Judge(Row row, const sg::Call& c) {
    if (Off()) return sg::Verdict::Run;
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    void* owner = R::OuterOf(c.object);
    if (!owner || !ue_wrap::prop::IsDescendantOfProp(owner)) return sg::Verdict::Run;
    if (!Strict() && AuthoredHere(owner)) {
        // A non-lethal call runs; a lethal one waits for the grant, since a break before it would delete the host's
        // prop even when the host refuses the election.
        if (AuthoredByPendingSeqOnly(owner) && Lethal(row, c)) return HoldLethal(row, c, owner);
        return sg::Verdict::Run;
    }

    // MTA lets the mirror break locally and sends nothing:
    // reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4894-4925. The server's is the shared break:
    // reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:8489-8505.
    // Here the call is cancelled instead, since a mirror that breaks alone stands apart from the host's prop until the
    // host's destroy arrives.
    ++g_cancelled[row];
    void* const cls = R::ClassOf(owner);
    if (!g_said[row].Find(cls)) {
        g_said[row].Insert(cls, true);
        UE_LOGI("prop_damage_gate: a client's %s on a prop it does not author is the host's (cls=%ls)", kVerb[row],
                R::ClassNameOf(owner).c_str());
    }
    return sg::Verdict::Cancel;
}

sg::Verdict OnImpactPre(const sg::Call& c) { return Judge(kImpact, c); }
sg::Verdict OnFirePre(const sg::Call& c) { return Judge(kFire, c); }

const sg::PreFn kPre[kRows] = {&OnImpactPre, &OnFirePre};

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRows] = {Reg::Pending, Reg::Pending};
bool g_settled[kRows] = {};

}  // namespace

bool AuthoredHere(void* prop) {
    void* me = coop::players::Registry::Get().Local();
    if (me && ue_wrap::engine::IsMainPlayerGrabbing(me, prop)) return true;
    if (coop::prop_throw::SeqFor(prop) != 0) return true;
    return coop::prop_syncer::AuthorsActorHere(prop);
}

void ReplayAtGrant(void* actor) {
    coop::prop_throw::DeferredBreak b;
    if (!coop::prop_throw::TakeBreak(actor, &b)) return;
    void* const comp = b.comp.Alive() ? b.comp.Get() : nullptr;
    void* const fn = comp ? R::FindFunction(R::ClassOf(comp), L"impactDamage") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) {
        UE_LOGW("prop_damage_gate: the held impactDamage cannot replay -- its component is gone or has no such function");
        return;
    }
    f.Set<float>(L"damage", b.damage);
    f.SetRaw(L"impact", b.impact, static_cast<int32_t>(sizeof(b.impact)));
    if (b.hitSize > 0) f.SetRaw(L"hit", b.hit, b.hitSize);
    f.Set<void*>(L"actor", b.causer.Alive() ? b.causer.Get() : nullptr);
    ++g_replayed;
    UE_LOGI("prop_damage_gate: the held impactDamage replays at its grant (damage=%.1f, causer %s) (#%llu)",
            static_cast<double>(b.damage), b.causer.Alive() ? "alive" : "gone", g_replayed);
    ue_wrap::Call(comp, f);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session || session->role() != coop::net::Role::Client) return;
    // A client cancels the call by these watches; the gate refuses a watch it cannot keep for the process.
    for (int i = 0; i < kRows; ++i)
        if (g_reg[i] == Reg::Pending)
            g_reg[i] = sg::WatchName(*kRowOf[i], kTag[i], kPre[i], nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    bool resolved = false;
    for (int i = 0; i < kRows; ++i) {
        if (g_settled[i] || g_reg[i] == Reg::Pending) continue;
        if (g_reg[i] == Reg::Registered && !resolved) {
            sg::ResolvePendingNames();
            resolved = true;
        }
        if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRowOf[i], kTag[i])) {
            g_settled[i] = true;
            UE_LOGI("prop_damage_gate: the comp_physicsImpact_C %s watch is live", kVerb[i]);
        } else if (g_reg[i] == Reg::Refused || sg::NameWatchSettled(*kRowOf[i], kTag[i])) {
            g_settled[i] = true;
            UE_LOGE("prop_damage_gate: the %s watch is dead -- a client's %s on a prop it does not author runs",
                    kVerb[i], kVerb[i]);
        }
    }
}

void OnDisconnect() {
    if (g_cancelled[kImpact] != 0 || g_cancelled[kFire] != 0)
        UE_LOGI("prop_damage_gate: %llu impactDamage and %llu fireDamage calls cancelled this session",
                g_cancelled[kImpact], g_cancelled[kFire]);
    if (g_held[kImpact] != 0 || g_held[kFire] != 0 || g_replayed != 0)
        UE_LOGI("prop_damage_gate: %llu lethal impactDamage held and %llu replayed, %llu lethal fireDamage dropped "
                "while a grant was awaited this session", g_held[kImpact], g_replayed, g_held[kFire]);
    g_replayed = 0;
    for (int i = 0; i < kRows; ++i) {
        g_cancelled[i] = 0;
        g_held[i] = 0;
        g_said[i].Clear();
        g_heldSaid[i].Clear();
    }
}

}  // namespace coop::props::prop_damage_gate
