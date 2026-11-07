// coop/interactables/woodchipper_gate.cpp -- see coop/interactables/woodchipper_gate.h.

#include "coop/interactables/woodchipper_gate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::woodchipper_gate {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace W  = ue_wrap::woodchipper;
using Payload = coop::net::WoodchipperPayload;

// The tick is the burn (fuel falls, and at 0 the machine switches itself off); the eat box's begin-overlap is the intake
// (the fed prop is destroyed, the clump spawns). The pour and the press are the two verbs a player's hands have on it.
UE_FACT_FN(kRowTick, "woodchipper_gate", L"woodchipper_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowEat, "woodchipper_gate", L"woodchipper_C",
           L"BndEvt__eat_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPour, "woodchipper_gate", L"woodchipper_C", L"gascanFuel", Declared);
UE_FACT_FN(kRowPress, "woodchipper_gate", L"woodchipper_C", L"actionOptionIndex", Declared);

enum Row { kTick, kEat, kPour, kPress, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowTick, &kRowEat, &kRowPour, &kRowPress};
const char* const kRowTag[kRowCount] = {"ReceiveTick", "eat.BeginOverlap", "gascanFuel", "actionOptionIndex"};
constexpr int kTagBase = 0x57470000;   // 'WG', then the row

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_watched{false};
std::atomic<uint32_t> g_registered{0};   // bit i: row i's watch registered
bool g_settled = false;                  // game thread: every registered row settled, and any dead one said

// The drill's red arms: the tick and intake refusals off (a client's copy burns and eats on its own), and the asks off
// (the client's pour and press reach no host).
bool GateOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::woodchipper_gate_off);
    return off;
}
bool IntentOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::woodchipper_intent_off);
    return off;
}

// Game thread, as the gate's callbacks are.
Counts g_counts;

// The ask of a client's player, sent to the host as the machine's intent. False when nothing was sent: the red arm, no
// running session, or a session that refused the message.
bool Ask(uint8_t op, const std::wstring& key, float amount, bool wanted) {
    if (IntentOff()) {
        ++g_counts.notSent;
        return false;
    }
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return false;
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = op;
    p.fuel = amount;
    p.active = wanted ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::WoodchipperIntent, &p, sizeof(p))) {
        ++g_counts.notSent;
        UE_LOGW("woodchipper_gate: the session refused the %s on '%ls'",
                op == coop::net::kWoodchipperOpPour ? "pour" : "press", key.c_str());
        return false;
    }
    return true;
}

bool WireKeyable(const std::wstring& key) { return !key.empty() && key.size() <= sizeof(coop::net::WireKey::data); }

// ---- the tick and the intake: refused on a client ----

// The host's intake, seen and never changed: what entered the eat box and what canChip will read of it. One line per
// (chipper, other actor) pair, the pairs capped, so a body resting in the box says nothing more.
struct EatPair {
    void* chipper;
    void* other;
};
std::vector<EatPair> g_eatPairs;
constexpr size_t kMaxEatPairs = 64;

void NoteHostEat(const sg::Call& call) {
    ++g_counts.eatHost;
    if (!call.locals) return;
    static R::ParamOffset sOther{L"OtherActor"}, sComp{L"OtherComp"};
    const int32_t otherOff = sOther.Of(call.function);
    const int32_t compOff = sComp.Of(call.function);
    if (otherOff < 0) return;
    void* const other = *reinterpret_cast<void* const*>(call.locals + otherOff);
    void* const comp = compOff < 0 ? nullptr : *reinterpret_cast<void* const*>(call.locals + compOff);
    for (const EatPair& e : g_eatPairs)
        if (e.chipper == call.object && e.other == other) return;
    if (g_eatPairs.size() >= kMaxEatPairs) return;
    g_eatPairs.push_back({call.object, other});
    float fuel = 0.f;
    bool active = false;
    W::Read(call.object, fuel, active);
    std::wstring key;
    if (other && R::IsLive(other)) {
        const ue_wrap::prop::KeyField where = ue_wrap::prop::KeyFieldOf(R::ClassOf(other));
        if (where != ue_wrap::prop::KeyField::None)
            key = R::ToString(ue_wrap::prop::ReadKeyField(other, where));
    }
    UE_LOGI("woodchipper_gate: HOST intake #%llu chipper %p (active %d, fuel %.1f) was entered by %p class '%ls' key '%ls' "
            "via component %p class '%ls' (simulating %d)", static_cast<unsigned long long>(g_counts.eatHost),
            call.object, active ? 1 : 0, static_cast<double>(fuel), other, R::ClassNameOf(other).c_str(), key.c_str(),
            comp, R::ClassNameOf(comp).c_str(), ue_wrap::engine::IsComponentSimulatingPhysics(comp) ? 1 : 0);
}

sg::Verdict OnRefusePre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (!coop::spawn_authority::IsActiveClientSession()) {
        if (row == kEat) NoteHostEat(call);
        return sg::Verdict::Run;
    }
    if (GateOff()) return sg::Verdict::Run;
    if (row != kTick && row != kEat) return sg::Verdict::Run;
    uint64_t& n = row == kTick ? g_counts.tickRefused : g_counts.eatRefused;
    ++n;
    if ((n & (n - 1)) == 0)
        UE_LOGI("woodchipper_gate[%s]: refuse %p (call #%llu) -- the host runs the chipper", kRowTag[row], call.object,
                static_cast<unsigned long long>(n));
    return sg::Verdict::Cancel;
}

// ---- the pour: the copy's own gascanFuel runs; the fuel it gained is asked of the host ----
// Divergence from MTA, whose client waits for the server's answer to a request (Packet_Vehicle_InOut, CGame.cpp:3018): a
// press waits below, but the pour cannot, since the can drains inside the game's own body. The copy is optimistic and the
// host's answer reverts it when the pour is refused (woodchipper_sync's Answer).

void* g_pourChipper = nullptr;
float g_pourBefore = 0.f;
std::vector<void*> g_emptyPours;   // the chippers a zero-gain pour was said for, once each

bool LocalPlayerArg(const sg::Call& call, R::ParamOffset& param) {
    const int32_t off = param.Of(call.function);
    if (off < 0 || !call.locals) return false;
    return coop::players::Registry::Get().IsLocal(*reinterpret_cast<void* const*>(call.locals + off));
}

sg::Verdict OnPourPre(const sg::Call& call) {
    g_pourChipper = nullptr;
    if (!coop::spawn_authority::IsActiveClientSession() || !W::IsChipper(call.object)) return sg::Verdict::Run;
    static R::ParamOffset sPlayer{L"player"};
    float fuel = 0.f;
    bool active = false;
    if (!LocalPlayerArg(call, sPlayer) || !W::Read(call.object, fuel, active)) return sg::Verdict::Run;
    g_pourChipper = call.object;
    g_pourBefore = fuel;
    return sg::Verdict::Run;
}

void OnPourPost(const sg::Call& call) {
    if (!g_pourChipper || call.object != g_pourChipper) return;
    g_pourChipper = nullptr;
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(call.object, fuel, active)) return;
    const float gained = std::fmin(fuel - g_pourBefore, W::kMaxFuel);
    if (!(gained > 0.f)) {   // the can was empty, or the tank full: nothing went in, so nothing is asked
        if (std::find(g_emptyPours.begin(), g_emptyPours.end(), call.object) == g_emptyPours.end()) {
            g_emptyPours.push_back(call.object);
            UE_LOGI("woodchipper_gate: this player's pour into chipper %p gained %.2f (tank %.2f -> %.2f) -- nothing is "
                    "asked of the host (said once per chipper)", call.object, static_cast<double>(gained),
                    static_cast<double>(g_pourBefore), static_cast<double>(fuel));
        }
        return;
    }
    const std::wstring key = W::KeyOf(call.object);
    if (!WireKeyable(key)) {
        UE_LOGW("woodchipper_gate: this player poured %.1f into chipper %p, which holds no key the host could resolve",
                gained, call.object);
        return;
    }
    if (Ask(coop::net::kWoodchipperOpPour, key, gained, false)) {
        ++g_counts.poursAsked;
        UE_LOGI("woodchipper_gate: this player poured %.1f into '%ls' -- the host adds it (#%llu)", gained, key.c_str(),
                static_cast<unsigned long long>(g_counts.poursAsked));
    }
}

// ---- the button: refused, and asked as the state the player wanted ----

sg::Verdict OnPressPre(const sg::Call& call) {
    if (!coop::spawn_authority::IsActiveClientSession() || !W::IsChipper(call.object) || !call.locals)
        return sg::Verdict::Run;
    static R::ParamOffset sAction{L"action"}, sPlayer{L"player"}, sHit{L"hit"};
    const int32_t actionOff = sAction.Of(call.function);
    const int32_t hitOff = sHit.Of(call.function);
    static bool sSaid = false;
    if (actionOff < 0 || hitOff < 0) {
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("woodchipper_gate: the press's action or hit parameter did not resolve -- a chipper press runs on "
                    "this copy alone (said once)");
        }
        return sg::Verdict::Run;
    }
    if (call.locals[actionOff] != W::kButtonAction || !LocalPlayerArg(call, sPlayer)) return sg::Verdict::Run;
    // The game toggles only when the hit names the button's box; a press elsewhere on the machine does nothing.
    void* const button = W::ButtonComponent(call.object);
    if (!button || ue_wrap::hit_result::Component(call.locals + hitOff) != button) return sg::Verdict::Run;
    float fuel = 0.f;
    bool active = false;
    const std::wstring key = W::KeyOf(call.object);
    if (!W::Read(call.object, fuel, active) || !WireKeyable(key)) {
        UE_LOGW("woodchipper_gate: a press on chipper %p, which holds no readable key or state -- nothing sent",
                call.object);
        return sg::Verdict::Cancel;
    }
    if (Ask(coop::net::kWoodchipperOpSetActive, key, 0.f, !active)) {
        ++g_counts.pressesAsked;
        UE_LOGI("woodchipper_gate: this player pressed '%ls' %s -- the host decides (#%llu)", key.c_str(),
                active ? "off" : "on", static_cast<unsigned long long>(g_counts.pressesAsked));
    }
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        sg::PreFn pre = i == kPour ? &OnPourPre : i == kPress ? &OnPressPre : &OnRefusePre;
        sg::PostFn post = i == kPour ? &OnPourPost : nullptr;
        if (sg::WatchName(*kRows[i], kTagBase + i, pre, post)) {
            g_registered.fetch_or(1u << i, std::memory_order_acq_rel);
            ++watched;
        } else {
            UE_LOGE("woodchipper_gate: the watch on %ls::%ls did not register -- a client runs it", kRows[i]->Class(),
                    kRows[i]->Name());
        }
    }
    UE_LOGI("woodchipper_gate: %d/%d chipper watches registered by class and name; the tick and the intake are "
            "refused on an active client session, its pour and press are asked of the host", watched, kRowCount);
}

void SettleWatches() {
    const uint32_t registered = g_registered.load(std::memory_order_acquire);
    if (g_settled || !registered) return;
    for (int i = 0; i < kRowCount; ++i)
        if ((registered & (1u << i)) && !sg::NameWatchSettled(*kRows[i], kTagBase + i)) return;
    g_settled = true;
    for (int i = 0; i < kRowCount; ++i)
        if ((registered & (1u << i)) && !sg::NameWatchLive(*kRows[i], kTagBase + i))
            UE_LOGW("woodchipper_gate: the watch on %ls::%ls settled dead -- a client chipper will run its own %s",
                    kRows[i]->Class(), kRows[i]->Name(), kRowTag[i]);
}

void OnDisconnect() {
    const Counts& c = g_counts;
    if (c.tickRefused || c.eatRefused || c.poursAsked || c.pressesAsked || c.notSent)
        UE_LOGI("woodchipper_gate: this session refused %llu chipper ticks and %llu intakes; asked %llu pours and %llu "
                "presses, %llu not sent", static_cast<unsigned long long>(c.tickRefused),
                static_cast<unsigned long long>(c.eatRefused), static_cast<unsigned long long>(c.poursAsked),
                static_cast<unsigned long long>(c.pressesAsked), static_cast<unsigned long long>(c.notSent));
    g_counts = Counts{};
    g_pourChipper = nullptr;
    g_emptyPours.clear();
    g_eatPairs.clear();
}

Counts GetCounts() { return g_counts; }

}  // namespace coop::woodchipper_gate
