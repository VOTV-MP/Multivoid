// coop/creatures/kerfus_intent.cpp -- see coop/creatures/kerfus_intent.h.

#include "coop/creatures/kerfus_intent.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/active_drive.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>

namespace coop::kerfus_intent {
namespace {

namespace EL = coop::element;
namespace OI = ue_wrap::object_index;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kKerfusClass = L"p_kerfus_C";
constexpr int kTagAction = 0x4B464901;  // 'KFI' 1
constexpr int kTagName   = 0x4B464902;

// The Kerfus's three verbs by action (its getActionOptions offers 8, 4, 6 when on and 8 when off).
constexpr uint8_t kActionUse    = 4;
constexpr uint8_t kActionPat    = 6;
constexpr uint8_t kActionToggle = 8;

// An arm's-length verb on a small robot, the console's reach.
constexpr float kKerfusReachUU = 400.0f;

// The game counts pats only while they come within 0.25 s of each other, so the bucket lets a client
// pat as fast as a player can; the bound is for a stalled client's backlog, not for a player.
constexpr float    kBurst      = 10.0f;
constexpr float    kPerSecond  = 10.0f;
constexpr size_t   kMaxPending = 16;
constexpr uint64_t kSayEveryMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_actionWatched = false;
bool g_nameWatched = false;

unsigned long long g_sent = 0, g_run = 0, g_denied = 0;
bool g_saidNoEid = false;
bool g_waitSaid[coop::net::kMaxPeers] = {};

struct Bucket {
    float    tokens = kBurst;
    uint64_t lastMs = 0;
    uint64_t nextSayMs = 0;
};
Bucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::KerfusIntentPayload> g_pending[coop::net::kMaxPeers];

bool IsVerb(uint8_t a) { return a == kActionUse || a == kActionPat || a == kActionToggle; }

bool IsKerfus(void* obj) {
    void* kerfus = OI::ClassByName(kKerfusClass);
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    return kerfus && cls && R::IsDescendantOfAny(cls, &kerfus, 1);
}

bool TakeToken(uint8_t slot) {
    Bucket& b = g_rate[slot];
    const uint64_t now = coop::active_drive::NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kBurst) b.tokens = kBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

// ---- the client's gate ---------------------------------------------------------------------------

coop::net::Session* ClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == coop::net::Role::Client) ? s : nullptr;
}

void Send(coop::net::Session* s, void* kerfus, uint8_t action) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(kerfus);
    if (eid == EL::kInvalidId) {
        if (!g_saidNoEid) {
            g_saidNoEid = true;
            UE_LOGW("kerfus_intent: a Kerfus verb on %p, which is no wire mirror -- refused, nothing to ask", kerfus);
        }
        return;
    }
    if (!s->connected()) return;
    coop::net::KerfusIntentPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.action = action;
    s->SendReliable(coop::net::ReliableKind::KerfusIntent, &p, sizeof(p));
    ++g_sent;
    if (g_sent <= 3 || g_sent % 50 == 0)
        UE_LOGI("kerfus_intent: CLIENT refused action %u on Kerfus eid=%u and asked the host (#%llu)",
                static_cast<unsigned>(action), static_cast<unsigned>(eid), g_sent);
}

sg::Verdict OnActionPre(const sg::Call& c) {
    auto* s = ClientSession();
    if (!s || !c.object) return sg::Verdict::Run;
    static void*   sFn = nullptr;
    static int32_t sActionOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sActionOff = R::FindParamOffset(c.function, L"action");
    }
    if (sActionOff < 0) return sg::Verdict::Run;
    const uint8_t action = c.locals[sActionOff];
    if (!IsVerb(action)) return sg::Verdict::Run;  // the colour variant's picker and anything else stay local
    Send(s, c.object, action);
    return sg::Verdict::Cancel;
}

// An FString param out of a frame: the 16-byte TArray<wchar_t> {Data, Num, Max}. Empty on null or an
// insane length.
std::wstring ReadFStringParam(const uint8_t* locals, int32_t off) {
    if (!locals || off < 0) return {};
    const uint8_t* base = locals + off;
    const wchar_t* data = *reinterpret_cast<const wchar_t* const*>(base);
    const int32_t num = *reinterpret_cast<const int32_t*>(base + 8);
    if (!data || num <= 1 || num > 64) return {};  // Num counts the terminator
    return std::wstring(data, static_cast<size_t>(num - 1));
}

sg::Verdict OnNamePre(const sg::Call& c) {
    auto* s = ClientSession();
    if (!s || !c.object) return sg::Verdict::Run;
    static void*   sFn = nullptr;
    static int32_t sNameOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sNameOff = R::FindParamOffset(c.function, L"name");
    }
    const std::wstring name = ReadFStringParam(c.locals, sNameOff);
    uint8_t action = 0;
    if (name == L"activate") action = kActionToggle;
    else if (name == L"use") action = kActionUse;
    else if (name == L"pat") action = kActionPat;
    else return sg::Verdict::Run;
    Send(s, c.object, action);
    return sg::Verdict::Cancel;
}

// ---- the host ------------------------------------------------------------------------------------

// One intent, run or refused and then consumed, or left at the head of its queue while the host has no
// body for the sender yet -- the seconds between a joiner's curtain and its first pose, a mid-join
// window rather than a verdict, which the console, door and keypad lanes wait out the same way.
bool Execute(coop::net::Session& s, const coop::net::KerfusIntentPayload& p, uint8_t slot) {
    const auto token = EL::IntentTarget::ForClientIntent(s, slot, kKerfusReachUU);
    const EL::IntentSubject subj = token.Resolve(static_cast<EL::ElementId>(p.elementId), EL::ElementType::Prop);
    if (subj.outcome == EL::IntentOutcome::NoBody) {
        if (!g_waitSaid[slot]) {
            g_waitSaid[slot] = true;
            UE_LOGI("kerfus_intent: slot %u's Kerfus verbs wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subj || !IsKerfus(subj.actor)) {
        ++g_denied;
        UE_LOGI("kerfus_intent: DENY slot=%u action=%u eid=%u -- %s", static_cast<unsigned>(slot),
                static_cast<unsigned>(p.action), p.elementId, subj ? "no Kerfus" : EL::OutcomeName(subj.outcome));
        return true;
    }
    void* k = subj.actor;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(k), L"actionOptionIndex");
    ue_wrap::ParamFrame f(fn);
    // The verbs read neither the player nor the hit (bp_cfg: no read of either in the ubergraph), so the
    // host's own player stands in; the Kerfus's own guards (energy, on, possessed) judge the press.
    if (!fn || !f.valid() || !f.Set<void*>(L"player", coop::players::Registry::Get().Local()) ||
        !f.Set<uint8_t>(L"action", p.action)) {
        ++g_denied;
        UE_LOGW("kerfus_intent: slot=%u -- the Kerfus's actionOptionIndex did not resolve", static_cast<unsigned>(slot));
        return true;
    }
    ue_wrap::Call(k, f);
    ++g_run;
    UE_LOGI("kerfus_intent: HOST ran action %u on Kerfus eid=%u for slot %u (#%llu)", static_cast<unsigned>(p.action),
            p.elementId, static_cast<unsigned>(slot), g_run);
    return true;
}

void Register() {
    if (!g_actionWatched)
        g_actionWatched = sg::WatchClassName(kKerfusClass, L"actionOptionIndex", kTagAction, &OnActionPre, nullptr);
    if (!g_nameWatched)
        g_nameWatched = sg::WatchClassName(kKerfusClass, L"actionName", kTagName, &OnNamePre, nullptr);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick(coop::net::Session& session) {
    Register();
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pending[slot].empty() || !TakeToken(slot)) continue;
        const coop::net::KerfusIntentPayload p = g_pending[slot].front();
        if (Execute(session, p, slot)) {
            g_pending[slot].pop_front();
            g_waitSaid[slot] = false;      // a consumed intent ends the wait's streak
        } else {
            g_rate[slot].tokens += 1.0f;   // a wait runs nothing, so it spends no token
        }
    }
}

void OnIntent(coop::net::Session& session, const coop::net::KerfusIntentPayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers || !IsVerb(payload.action)) return;
    auto& q = g_pending[senderSlot];
    if (q.size() >= kMaxPending) {
        Bucket& b = g_rate[senderSlot];
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= b.nextSayMs) {
            b.nextSayMs = now + kSayEveryMs;
            UE_LOGW("kerfus_intent: slot %u queue full (%zu) -- refusing verbs until it drains",
                    static_cast<unsigned>(senderSlot), q.size());
        }
        ++g_denied;
        return;
    }
    q.push_back(payload);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_pending[slot].clear();
    g_rate[slot] = Bucket{};
    g_waitSaid[slot] = false;
}

void OnDisconnect() {
    if (g_sent || g_run || g_denied)
        UE_LOGI("kerfus_intent: session end -- sent=%llu run=%llu denied=%llu", g_sent, g_run, g_denied);
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) OnPeerLeft(slot);
    g_sent = g_run = g_denied = 0;
    g_saidNoEid = false;
}

unsigned long long SentCount() { return g_sent; }

}  // namespace coop::kerfus_intent
