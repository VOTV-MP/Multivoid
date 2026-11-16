// coop/props/hacksaw_cut.cpp -- see coop/props/hacksaw_cut.h.

#include "coop/props/hacksaw_cut.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/props/container_break.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <cstdint>
#include <string>

namespace coop::props::hacksaw_cut {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The saw's own tick and the end of its cut, both script bodies of prop_hacksaw_C, and the prop's `broken` on every class
// that declares it (the saw calls it on the prop it saws through a context object, inside its own graph).
UE_FACT_FN(kRowTick, "hacksaw_cut", L"prop_hacksaw_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowComplete, "hacksaw_cut", L"prop_hacksaw_C", L"completeSaw", Declared);
UE_FACT_FN(kRowBroken, "hacksaw_cut", nullptr, L"broken", AnyClass);
UE_FACT_FN(kRowUse, "hacksaw_cut", L"prop_hacksaw_C", L"playerHandUse_LMB", Declared);
constexpr int kTagTick     = 0x4853544B;   // 'HSTK'
constexpr int kTagComplete = 0x48534344;   // 'HSCD'
constexpr int kTagBroken   = 0x48534B42;   // 'HSKB'
constexpr int kTagUse      = 0x48535553;   // 'HSUS'
constexpr int kMaxTries = 600;
constexpr uint8_t kHintRefusal = 1;
constexpr const wchar_t* kHintText = L"Cannot saw this prop!";   // the Blueprint's own text for a prop it will not take

// The one cut this client's saw has asked about: its prop's Key, and the saw (a slot and serial, never a bare pointer).
struct Cut {
    bool active = false;
    bool granted = false;
    std::wstring propKey;
    ue_wrap::CachedObjRef saw, prop;   // the prop the saw was cutting when it asked
};
Cut g_cut;
R::InstanceOffset g_sawingPropOff{L"sawingProp"};
uint32_t g_world = 0;
uint32_t g_asked = 0, g_granted = 0, g_refused = 0, g_completed = 0, g_brokenCancelled = 0, g_strayAnswers = 0;
uint32_t g_tickCalls = 0, g_tickCancels = 0, g_withdrawn = 0;

std::wstring KeyOf(void* a) {
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(a);
    if (key.empty() || key == L"None" || key.size() >= sizeof(coop::net::WireKey::data)) return {};
    return key;
}

bool OnActiveClient() { return coop::spawn_authority::IsActiveClientSession(); }

// The game's own answer for a cut it will not make: its hint, then the saw's own end of the cut, which clears its tick,
// its target, its timer, its bar and its widget, so the saw rests and nothing asks again.
void AnswerRefusal(void* saw) {
    ue_wrap::votv_lib::AddHint(kHintText, kHintRefusal, saw);
    ue_wrap::component_calls::CallParamlessNamed(saw, L"completeSaw");
}

// A grant the host holds for a cut this saw will not finish is handed back (container_break op 4): the host's record of it
// ends with the cut, not with a destroy that never comes, so another saw can be granted the prop and a later destroy of it
// is not broken first. A saw's own session end needs none: the host ends the record when the slot leaves.
void WithdrawGrant(const std::wstring& propKey, const char* why) {
    ++g_withdrawn;
    const bool queued = coop::props::container_break::WithdrawSaw(propKey);
    UE_LOGI("hacksaw_cut: the granted cut of '%ls' ended without its destroy (%s) -- withdrawn from the host%s (#%u)",
            propKey.c_str(), why, queued ? "" : " (the queue is full: not sent)", g_withdrawn);
}

float ReadFloat(const uint8_t* at) { return *reinterpret_cast<const float*>(at); }

// The saw's tick, on a client, before its body. The sum is the Blueprint's own (offsets 4102-4185: DeltaSeconds divided by
// `time`, added to `complete`, compared with 1, each step in float32), so a tick this reads as completing is one the body
// would complete; the drill's host-born spill and its client-born count say when it is not.
sg::Verdict TickVerdict(const sg::Call& c) {
    if (!c.object || !c.locals || !OnActiveClient()) return sg::Verdict::Run;
    // The drill's red: the cut runs on this copy and asks the host nothing.
    static const bool s_off = coop::config::ResolveFlag(coop::config_registry::rows::hacksaw_request_off);
    if (s_off) return sg::Verdict::Run;
    static R::InstanceOffset s_complete{L"complete"}, s_time{L"time"};
    static R::ParamOffset s_dt{L"DeltaSeconds"};
    const int32_t oProp = g_sawingPropOff.Of(c.object), oComplete = s_complete.Of(c.object), oTime = s_time.Of(c.object),
                  oDt = s_dt.Of(c.function);
    if (oProp < 0 || oComplete < 0 || oTime < 0 || oDt < 0) return sg::Verdict::Run;
    const uint8_t* self = static_cast<const uint8_t*>(c.object);
    void* prop = *reinterpret_cast<void* const*>(self + oProp);
    const bool latched = g_cut.active && g_cut.saw.Is(c.object);
    // The cut's prop is gone or its target cleared while a request waits: the tick stays cancelled until the saw ends the
    // cut (its own timer or the trigger's release), which the POST on completeSaw sees.
    if (!prop || !R::IsLive(prop)) return latched ? sg::Verdict::Cancel : sg::Verdict::Run;
    const float time = ReadFloat(self + oTime);
    if (!(time > 0.f)) return sg::Verdict::Run;
    const float add = ReadFloat(c.locals + oDt) / time;
    const float sum = ReadFloat(self + oComplete) + add;
    if (!(sum >= 1.f)) return sg::Verdict::Run;
    // An unkeyed prop is not shared state (no peer holds a copy to agree with): its cut stays this copy's.
    const std::wstring propKey = KeyOf(prop);
    if (propKey.empty()) return sg::Verdict::Run;
    if (latched && g_cut.propKey == propKey) return g_cut.granted ? sg::Verdict::Run : sg::Verdict::Cancel;
    g_cut = Cut{};   // the latch of an earlier cut whose saw is gone
    if (!coop::props::container_break::RequestSaw(propKey)) {
        UE_LOGW("hacksaw_cut: this client's queue of requests is full -- the cut of '%ls' is answered here as a refusal",
                propKey.c_str());
        AnswerRefusal(c.object);
        return sg::Verdict::Cancel;
    }
    g_cut.active = true;
    g_cut.propKey = propKey;
    g_cut.saw.Set(c.object);
    g_cut.prop.Set(prop);
    ++g_asked;
    UE_LOGI("hacksaw_cut: this player's saw would complete its cut of '%ls' -- the tick is cancelled until the host grants it "
            "(#%u)", propKey.c_str(), g_asked);
    return sg::Verdict::Cancel;
}

// The saw's tick, counted: how many times the gate called the PRE on a client and how many of them it cancelled, which
// the drill reads to tell a tick that never runs from one this PRE holds back.
sg::Verdict OnTickPre(const sg::Call& c) {
    if (c.object && OnActiveClient()) ++g_tickCalls;
    const sg::Verdict v = TickVerdict(c);
    if (v == sg::Verdict::Cancel) ++g_tickCancels;
    return v;
}

// The saw's start of a cut. playerHandUse_LMB is one ubergraph call (ubergraph entry 4853) that runs the
// start gates and the start itself without a latent step: the player's hit actor and its safeAsProp, the
// StaticMesh's IsSimulatingPhysics, propData.heavy, processMat, isNotSawable, then
// SetActorTickEnabled(true) and the start at the label after them (the timer, the volume, `time`, the widget). A coop copy of
// a prop at rest is not simulating on a client, so the game's own gate refuses it ("Cannot saw the static prop!") with the
// target already set and the tick off.
//
// INTERIM, per the tracker row MIRROR-SIMULATING-GATES: a census of the IsSimulatingPhysics reads in client-run graphs,
// then one root decision on a client copy's simulation. That decision retires this flip, as it retires the chipper's in
// woodchipper_gate. Until then the PRE sets the target's copy simulating for this one call when it is a prop mirror that is
// not and stands free, and the POST sets it back whatever the gates answered; a POST the gate skipped is caught by Tick, which
// sees the use body is no longer running. Nothing else of the copy is touched.
struct Flip {
    bool active = false;
    void* fn = nullptr;   // the use function, whose body running is what the flip lasts for
    std::wstring key;
    ue_wrap::CachedObjRef saw, comp, actor;
};
Flip g_flip;
uint32_t g_flipped = 0, g_restored = 0;

void RestoreFlip(const char* why) {
    if (!g_flip.active) return;
    void* comp = g_flip.comp.Get();
    // A copy that is attached now (the game glued it while the body ran) is the game's to leave: the flip is only undone on a
    // body that stands free, as it was found.
    void* parent = nullptr;
    void* actor = g_flip.actor.Get();
    const bool attached = actor && ue_wrap::engine::GetAttachParentActor(actor, parent) && parent;
    const bool ok = comp && !attached && ue_wrap::engine::SetComponentSimulatePhysics(comp, false);
    ++g_restored;
    UE_LOGI("hacksaw_cut: the copy of '%ls' the saw's start set simulating is set back to not simulating (%s; restored=%d%s, "
            "#%u)", g_flip.key.c_str(), why, ok ? 1 : 0, attached ? ", left: it is attached now" : "", g_restored);
    g_flip = Flip{};
}

sg::Verdict OnUsePre(const sg::Call& c) {
    if (!c.object || !c.locals || !OnActiveClient()) return sg::Verdict::Run;
    if (g_flip.active) RestoreFlip("an earlier use left no POST");
    static R::ParamOffset s_player{L"player"};
    const int32_t oPlayer = s_player.Of(c.function);
    if (oPlayer < 0) return sg::Verdict::Run;
    void* player = *reinterpret_cast<void* const*>(c.locals + oPlayer);
    void* hit = player ? ue_wrap::engine::ReadMainPlayerHitActor(player) : nullptr;
    if (!hit || !R::IsLive(hit)) return sg::Verdict::Run;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(hit);
    const coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    if (!el || !el->IsMirror() || el->GetType() != coop::element::ElementType::Prop) return sg::Verdict::Run;
    void* mesh = ue_wrap::prop::GetStaticMesh(hit);
    if (!mesh || ue_wrap::engine::IsComponentSimulatingPhysics(mesh)) return sg::Verdict::Run;
    // A mirror attached to another actor (glued in a bin, held) would be detached by a simulating body: the gate refuses it as
    // it would the game's own, with nothing flipped.
    void* parent = nullptr;
    if (ue_wrap::engine::GetAttachParentActor(hit, parent) && parent) return sg::Verdict::Run;
    if (!ue_wrap::engine::SetComponentSimulatePhysics(mesh, true)) return sg::Verdict::Run;
    g_flip.active = true;
    g_flip.fn = c.function;
    g_flip.key = KeyOf(hit);
    g_flip.saw.Set(c.object);
    g_flip.comp.Set(mesh);
    g_flip.actor.Set(hit);
    ++g_flipped;
    UE_LOGI("hacksaw_cut: the saw's start meets the copy of '%ls' (%ls, eid %u) not simulating, as a resting host prop's copy "
            "is on a client -- set simulating for this call so the game's own gate judges it as on its own prop (#%u)",
            g_flip.key.c_str(), R::ClassNameOf(hit).c_str(), static_cast<unsigned>(eid), g_flipped);
    return sg::Verdict::Run;
}

void OnUsePost(const sg::Call& c) {
    if (!g_flip.active || !g_flip.saw.Is(c.object)) return;
    RestoreFlip("the saw's use ended");
}

// The end of the saw's cut, by its own completion, its timer or the trigger's release, or ours after a refusal.
void OnCompletePost(const sg::Call& c) {
    if (!g_cut.active || !g_cut.saw.Is(c.object)) return;
    // The completion destroys the prop before it ends the cut, so a granted cut whose prop still stands ended some other way
    // (the trigger's release, the aim off the prop, the saw dropped): its grant is handed back.
    void* prop = g_cut.prop.Get();
    const bool destroyed = !prop || !R::IsLive(prop);
    if (g_cut.granted && destroyed) ++g_completed;
    UE_LOGI("hacksaw_cut: the saw ended its cut of '%ls' (%s)", g_cut.propKey.c_str(),
            !g_cut.granted ? "no grant came" : destroyed ? "granted, run to its end" : "granted, the prop still stands");
    if (g_cut.granted && !destroyed) WithdrawGrant(g_cut.propKey, "completeSaw with the prop standing");
    g_cut = Cut{};
}

// The prop's `broken`, once its cut is granted: the host runs it on its copy when this saw's destroy arrives, so this
// copy's body does not, and no spill is born here.
sg::Verdict OnBrokenPre(const sg::Call& c) {
    if (!g_cut.active || !g_cut.granted || !c.object || !OnActiveClient()) return sg::Verdict::Run;
    if (KeyOf(c.object) != g_cut.propKey) return sg::Verdict::Run;
    ++g_brokenCancelled;
    UE_LOGI("hacksaw_cut: the granted prop '%ls' is not broken here -- the host breaks its copy when the destroy arrives (#%u)", g_cut.propKey.c_str(),
            g_brokenCancelled);
    return sg::Verdict::Cancel;
}

struct Watch {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
    bool registered;
};
Watch g_watches[] = {
    {&kRowTick, kTagTick, &OnTickPre, nullptr, false},
    {&kRowComplete, kTagComplete, nullptr, &OnCompletePost, false},
    {&kRowBroken, kTagBroken, &OnBrokenPre, nullptr, false},
    {&kRowUse, kTagUse, &OnUsePre, &OnUsePost, false},
};
bool g_settled = false;
int g_tries = 0;

}  // namespace

// The saw's four watches stand together or not at all: a PRE that cancels the tick with no POST to end its latch, or a latch
// with no flip to let the saw start, would hold the saw back for good. So a give-up retires every watch that did register,
// and the saw runs wholly native, as the warning says.
namespace {
void DisarmAll() {
    for (Watch& w : g_watches) {
        if (w.registered) sg::UnwatchName(*w.row, w.tag, w.pre, w.post);
        w.registered = false;
    }
}
}  // namespace

// The watches are armed on a client session only: the host and solo play never run them (the callbacks refuse a host too).
void Install() {
    if (g_settled || !OnActiveClient()) return;
    bool all = true;
    for (Watch& w : g_watches) {
        if (!w.registered) w.registered = sg::WatchName(*w.row, w.tag, w.pre, w.post);
        all = all && w.registered;
    }
    if (!all) {
        if (++g_tries >= kMaxTries) {
            g_settled = true;
            DisarmAll();
            UE_LOGW("hacksaw_cut: the gate took no watch on the saw -- its cut runs on this client unasked");
        }
        return;
    }
    sg::ResolvePendingNames();
    for (const Watch& w : g_watches)
        if (!sg::NameWatchSettled(*w.row, w.tag)) return;
    g_settled = true;
    for (const Watch& w : g_watches) {
        if (sg::NameWatchLive(*w.row, w.tag)) continue;
        DisarmAll();
        UE_LOGW("hacksaw_cut: the watch on %ls settled dead -- the saw's cut is not judged by the host", w.row->Name());
        return;
    }
    UE_LOGI("hacksaw_cut: the saw's ReceiveTick, completeSaw and playerHandUse_LMB and `broken` are watched");
}

void OnGranted(const std::wstring& propKey) {
    if (g_cut.active && !g_cut.granted && g_cut.propKey == propKey) {
        g_cut.granted = true;
        ++g_granted;
        UE_LOGI("hacksaw_cut: the host granted the cut of '%ls' -- the saw runs it on its next tick (#%u)", propKey.c_str(),
                g_granted);
        return;
    }
    if (g_cut.active && g_cut.granted && g_cut.propKey == propKey) {
        UE_LOGW("hacksaw_cut: the host granted the cut of '%ls' a second time -- the live grant stands", propKey.c_str());
        return;
    }
    ++g_strayAnswers;
    UE_LOGW("hacksaw_cut: the host granted the cut of '%ls', which no saw here waits for (the cut ended first): nothing was "
            "broken there", propKey.c_str());
    WithdrawGrant(propKey, "the grant came after the cut ended");
}

void OnRefused(const std::wstring& propKey) {
    if (!g_cut.active || g_cut.granted || g_cut.propKey != propKey) {
        ++g_strayAnswers;
        UE_LOGW("hacksaw_cut: the host refused the cut of '%ls', which no saw here waits for", propKey.c_str());
        return;
    }
    ++g_refused;
    void* saw = g_cut.saw.Get();
    g_cut = Cut{};
    if (saw) AnswerRefusal(saw);
}

uint32_t TickCalls() { return g_tickCalls; }
uint32_t TickCancels() { return g_tickCancels; }

// A granted cut whose saw no longer cuts its prop (the saw left the hand and was destroyed, its target changed, the prop
// stands): the grant is handed back. A prop that is gone ends the host's record by itself (it prunes a dead prop).
void CheckGrantedLatch() {
    if (!g_cut.active || !g_cut.granted) return;
    void* saw = g_cut.saw.Get();
    void* prop = g_cut.prop.Get();
    const int32_t o = saw ? g_sawingPropOff.Of(saw) : -1;
    void* target = o >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(saw) + o) : nullptr;
    if (!prop || !R::IsLive(prop)) return;
    if (saw && target == prop) return;
    WithdrawGrant(g_cut.propKey, saw ? "the saw's target changed" : "the saw is gone");
    g_cut = Cut{};
}

void Tick() {
    // The saw's use ended and its POST did not come (a fault the gate absorbed, a cancel by another watch): the body is not
    // running, so the copy the PRE set simulating goes back now.
    if (g_flip.active && !sg::IsBodyActive(g_flip.fn)) RestoreFlip("the saw's use ended without its POST");
    CheckGrantedLatch();
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world == g_world) return;
    g_world = world;
    RestoreFlip("the world changed");
    if (g_cut.active && g_cut.granted) WithdrawGrant(g_cut.propKey, "the world changed");
    g_cut = Cut{};
}

void OnDisconnect() {
    RestoreFlip("the session ended");
    if (g_asked || g_granted || g_refused || g_completed || g_brokenCancelled || g_strayAnswers)
        UE_LOGI("hacksaw_cut: session end -- asked=%u granted=%u refused=%u completed=%u, broken cancelled=%u, stray answers=%u",
                g_asked, g_granted, g_refused, g_completed, g_brokenCancelled, g_strayAnswers);
    g_asked = g_granted = g_refused = g_completed = g_brokenCancelled = g_strayAnswers = 0;
    g_tickCalls = g_tickCancels = g_flipped = g_restored = g_withdrawn = 0;
    g_cut = Cut{};
    g_world = 0;
}

}  // namespace coop::props::hacksaw_cut
