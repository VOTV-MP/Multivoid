// coop/creatures/kerfur_convert.cpp -- the kerfur conversion (NPC to prop and back) decided at its
// verb: a client's gate refuses the verb and asks the host, the host's verb converges at its return.
// The host executor lives in kerfur_convert_host.cpp, the client apply in kerfur_convert_client.cpp.
// See coop/creatures/kerfur_convert.h.

#include "coop/creatures/kerfur_convert.h"

#include "coop/creatures/kerfur_color_sync.h"      // a new form's colour
#include "coop/creatures/kerfur_convert_client.h"  // the wire apply
#include "coop/creatures/kerfur_convert_host.h"    // the converge and the request execution
#include "coop/creatures/kerfur_entity.h"          // the resolved classes, ReleaseKerfurForEid
#include "coop/creatures/kerfur_form_assembler.h"  // HasCapturedForm
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::kerfur_convert {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};

coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// Resolved engine refs: written by Install on the game thread before the gate watches register, read-only after.
// The kerfur classes are loaded at the main menu and live for the process (coop/dev/class_lifetime_probe measures
// it), so these refs and the watches hold across a world change.
void* g_kerfurNpcClass  = nullptr;  // kerfurOmega_C (the NPC base; ~20 data-only skin subclasses)
void* g_kerfurPropClass = nullptr;  // prop_kerfurOmega_C (the prop base; skins likewise)
int32_t g_killOff          = -1;  // kerfurOmega_C::kill bool (the BP's own turn_off guard)
// The base verb declarers, whose signatures the install checks. dropKerfurProp is overridden by the
// collar variants; the host runs the most-derived declarer, looked up on the target's own class.
void* g_dropPropFnBase     = nullptr;  // kerfurOmega_C::dropKerfurProp
void* g_spawnKerfuroFn     = nullptr;  // prop_kerfurOmega_C::spawnKerfuro (sole declarer)

// This lane's gate watches on the two verbs; the form assembler keeps its own on the same names.
constexpr const wchar_t* kTurnOffVerb = L"dropKerfurProp";
constexpr const wchar_t* kTurnOnVerb  = L"spawnKerfuro";
UE_FACT_FN(kRowDropKerfurProp, "kerfur_convert", nullptr, L"dropKerfurProp", AnyClass);
UE_FACT_FN(kRowSpawnKerfuro, "kerfur_convert", nullptr, L"spawnKerfuro", AnyClass);
constexpr int kWatchTurnOff = 0x4B430001;  // 'KC' 1
// The gamer Omega's prop and the custom-colour Omega's (prop_kerfurOmega_col_gamer_C, prop_kerfurOmega_col_C) turn on
// in their own actionOptionIndex: action 8 spawns the Omega itself, with the prop's Type and sentient (and the colour's
// hsv), and never calls spawnKerfuro, so this lane never saw it -- the host's Omega reached no client, and a client's
// turned on on its screen alone (VOTV-MP/Multivoid#185). In a session that action is refused and spawnKerfuro called
// in its place, with the prop's spawnKerfur (which these props leave empty) set to its Omega, so it converts as every
// other kerfur does; the host's return writes the prop's fields on the new Omega, as the prop's body does.
UE_FACT_FN(kRowActionOptionIndex, "kerfur_convert", nullptr, L"actionOptionIndex", AnyClass);
constexpr int kWatchGamerUse = 0x4B430003;  // 'KC' 3
constexpr uint8_t kActionTurnOn = 8;        // the interaction actions' turn-on, as the props' graphs compare it
struct InlineTurnOn {
    const wchar_t* prop;
    const wchar_t* npc;
    bool hsv;           // the colour's: the Omega's hsv, painted by its SetStyle
    void* propClass;
    void* npcClass;
};
InlineTurnOn g_inline[] = {
    {L"prop_kerfurOmega_col_gamer_C", L"kerfurOmega_col_gamer_C", false, nullptr, nullptr},
    {L"prop_kerfurOmega_col_C", L"kerfurOmega_col_C", true, nullptr, nullptr},
};
void* g_gamerNpcClass = nullptr;   // the gamer Omega, whose copies PaintGamerCopies paints
constexpr int kWatchTurnOn  = 0x4B430002;

// ---- the verb, on a client: refused, and asked of the host ----------------------------------

// A client never converts: the host owns which form a kerfur has. The player's own press reaches
// the verb from the kerfur's own graph (the radial menu's turn_off, the prop's E-press), and a drill
// standing in for it through ProcessEvent with no calling frame; both are asked of the host. Another
// actor's graph -- the gray event, the UFO dropper -- is refused without a request: the host's copy of
// that event converts the host's kerfur. Game thread.
bool g_saidEventRefusal = false;

void RefuseOnClient(coop::net::Session* s, const sg::Call& c, bool toProp) {
    const char* what = toProp ? "turn_off" : "turn-on";
    const bool press = !c.callerObject || c.callerObject == c.object;
    if (!press) {
        if (!g_saidEventRefusal) {
            g_saidEventRefusal = true;
            const std::wstring caller = R::ClassNameOf(c.callerObject);
            UE_LOGI("kerfur_convert[client]: %s from %ls's graph refused -- the host's own copy of it converts "
                    "the host's kerfur (said once per session)", what, caller.c_str());
        }
        return;
    }
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    if (eid == EL::kInvalidId) {
        UE_LOGW("kerfur_convert[client]: %s refused on %p, which is no wire mirror -- nothing to ask the host for",
                what, c.object);
        return;
    }
    if (!s->connected()) return;
    coop::net::KerfurConvertPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.toProp = toProp ? 1 : 0;
    s->SendReliable(coop::net::ReliableKind::KerfurConvertRequest, &p, sizeof(p));
    UE_LOGI("kerfur_convert[client]: %s refused here and asked of the host (eid=%u); its KerfurConvert brings "
            "the new form", what, static_cast<unsigned>(eid));
}

// ---- the verb, on the host: its bracket, and the converge at its return ----------------------

// The conversions whose verb body is running, by the verb's object, so the return finds what the
// entry saw: the old form's element id (a prop form's element is drained inside the verb, at its
// destroy seam), the verb function for the gate's own scope test, and the slot whose request runs it.
// Keyed on the object rather than counted: a body that faults, or that another consumer cancels,
// skips its return, and the entry it leaves is replaced by the next verb on that object. Game thread.
struct OpenVerb {
    void*         object;
    int32_t       idx;
    void*         function;
    EL::ElementId eid;
    bool          toProp;
    int           requestSlot;
};
std::vector<OpenVerb> g_open;

// The inline turn-on `actor` is the prop of, or null.
const InlineTurnOn* InlineOf(void* actor) {
    void* const cls = actor ? R::ClassOf(actor) : nullptr;
    for (const InlineTurnOn& k : g_inline)
        if (cls && k.propClass && k.npcClass && cls == k.propClass) return &k;
    return nullptr;
}

// The prop's spawnKerfur set to its Omega, which spawnKerfuro spawns. False when it cannot be.
bool PointInlineSpawn(void* prop, const InlineTurnOn& k) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(prop), L"spawnKerfur");
    if (off < 0) return false;
    *reinterpret_cast<void**>(static_cast<char*>(prop) + off) = k.npcClass;
    return true;
}

// The host's turn-on of such a prop: the prop's Type, sentient and hsv, for its return to write on the new Omega.
struct InlineCarry {
    void* prop = nullptr;
    const InlineTurnOn* kind = nullptr;
    int32_t type = 0;
    uint8_t sentient = 0;
    ue_wrap::FVector hsv{};
};
InlineCarry g_carry;

std::vector<void*> NpcsOf(void* cls) {
    std::vector<void*> out;
    if (cls)
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}
std::vector<void*> GamerNpcs() { return NpcsOf(g_gamerNpcClass); }

template <class T>
T Field(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}
template <class T>
void SetField(void* obj, const wchar_t* name, T v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off >= 0) *reinterpret_cast<T*>(static_cast<char*>(obj) + off) = v;
}

// Such a prop's action 8 in a session: refused, and spawnKerfuro run on the prop instead, whose watch below asks the
// host (a client) or converts it (the host). Any other action, and a solo game, run the game's own.
sg::Verdict OnGamerUsePre(const sg::Call& c) {
    auto* s = LoadSession();
    const InlineTurnOn* const k = InlineOf(c.object);
    if (!s || !s->running() || !k) return sg::Verdict::Run;
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"action") : -1;
    if (off < 0 || c.locals[off] != kActionTurnOn) return sg::Verdict::Run;
    if (!PointInlineSpawn(c.object, *k) || !g_spawnKerfuroFn) return sg::Verdict::Run;
    ue_wrap::ParamFrame f(g_spawnKerfuroFn);
    const bool ran = f.valid() && ue_wrap::Call(c.object, f);
    UE_LOGI("kerfur_convert: %ls's own turn-on on %p is run as spawnKerfuro (ran=%d)", k->prop, c.object, ran ? 1 : 0);
    return sg::Verdict::Cancel;
}

sg::Verdict OnVerbPre(const sg::Call& c) {
    auto* s = LoadSession();
    if (!s || !s->running()) return sg::Verdict::Run;
    const bool toProp = (c.tag == kWatchTurnOff);
    if (s->role() == coop::net::Role::Client) {
        RefuseOnClient(s, c, toProp);
        return sg::Verdict::Cancel;
    }
    if (const InlineTurnOn* k = toProp ? nullptr : InlineOf(c.object)) {   // a request runs spawnKerfuro straight
        PointInlineSpawn(c.object, *k);
        g_carry.prop = c.object;
        g_carry.kind = k;
        g_carry.type = Field<int32_t>(c.object, L"Type", 0);
        g_carry.sentient = Field<uint8_t>(c.object, L"sentient", 0);
        g_carry.hsv = Field<ue_wrap::FVector>(c.object, L"hsv", ue_wrap::FVector{});
    }
    const OpenVerb entry{c.object, R::InternalIndexOf(c.object), c.function,
                         EL::Registry::Get().EidForActor(c.object), toProp,
                         coop::kerfur_convert_host::RequestSlotFor(c.object)};
    for (OpenVerb& o : g_open) {
        if (o.object == c.object) { o = entry; return sg::Verdict::Run; }
    }
    g_open.push_back(entry);
    return sg::Verdict::Run;
}

void OnVerbPost(const sg::Call& c) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    for (auto it = g_open.begin(); it != g_open.end(); ++it) {
        if (it->object != c.object) continue;
        const OpenVerb o = *it;
        g_open.erase(it);
        void* const fresh = coop::kerfur_convert_host::ConvergeAtReturn(o.object, o.idx, o.eid, o.toProp, o.requestSlot);
        const bool carried = !o.toProp && g_carry.prop == o.object && g_carry.kind;
        if (carried && fresh && R::ClassOf(fresh) == g_carry.kind->npcClass) {   // the prop body's writes on its Omega
            SetField<int32_t>(fresh, L"Type", g_carry.type);
            SetField<uint8_t>(fresh, L"sentient", g_carry.sentient);
            if (g_carry.kind->hsv) {
                // The body sets hsv before the Omega's construction paints it; here it lands after, so it is painted.
                SetField<ue_wrap::FVector>(fresh, L"hsv", g_carry.hsv);
                void* const paint = R::FindDispatchFunctionCached(R::ClassOf(fresh), L"SetStyle");
                ue_wrap::ParamFrame f(paint);
                if (paint && f.valid()) ue_wrap::Call(fresh, f);
            }
        }
        if (carried) g_carry = InlineCarry{};
        // A custom-colour kerfur's new form took its colour from the old on the host alone: it goes out behind the convert.
        if (fresh) coop::kerfur_color_sync::HostAnnounce(fresh);
        return;
    }
}

// Is `actor` the object of a conversion verb whose body is running on this thread? The entry says
// which verb; the gate's own scope says it is still running, which an entry a faulted body left
// behind is not.
bool InOwnVerb(void* actor) {
    for (const OpenVerb& o : g_open)
        if (o.object == actor) return sg::IsBodyActive(o.function);
    return false;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::kerfur_convert_client::SetSession(session);  // both halves mirror the store above
    coop::kerfur_convert_host::SetSession(session);
    if (g_installed.load(std::memory_order_acquire)) return;
    // One index probe a call until both base classes are found. From then the install settles on
    // this call, whatever resolves: a Blueprint class loads with its functions, and a full table is a
    // capacity fault to fix, not a slot to wait for.
    if (!g_kerfurNpcClass)  g_kerfurNpcClass  = ue_wrap::object_index::ClassByName(L"kerfurOmega_C");
    if (!g_kerfurPropClass) g_kerfurPropClass = ue_wrap::object_index::ClassByName(L"prop_kerfurOmega_C");
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return;
    g_installed.store(true, std::memory_order_release);
    // The resolved bases are shared with kerfur_entity, so its class gates answer without
    // re-resolving.
    coop::kerfur_entity::SetKerfurClasses(g_kerfurNpcClass, g_kerfurPropClass);

    if (!g_dropPropFnBase) g_dropPropFnBase = R::FindFunction(g_kerfurNpcClass, kTurnOffVerb);
    if (!g_spawnKerfuroFn) g_spawnKerfuroFn = R::FindFunction(g_kerfurPropClass, kTurnOnVerb);
    if (g_killOff < 0)     g_killOff = R::FindPropertyOffset(g_kerfurNpcClass, L"kill");

    coop::kerfur_convert_host::SetClasses(g_kerfurNpcClass, g_kerfurPropClass);

    if (!g_dropPropFnBase || !g_spawnKerfuroFn) {
        UE_LOGE("kerfur_convert: partial resolve (drop=%p spawn=%p) -- module DISABLED (a recooked kerfur Blueprint)",
                g_dropPropFnBase, g_spawnKerfuroFn);
        return;
    }
    if (g_killOff < 0) {
        // Non-fatal: the guard read degrades to no guard (a murder-mode kerfur could be turned off
        // by a request; single-player denies it).
        UE_LOGW("kerfur_convert: kerfurOmega_C 'kill' offset unresolved -- BP murder-guard not replicated");
    }
    // The host hands the verbs a zeroed 16-byte frame because they take no params; should a game
    // update add one, the module refuses to install rather than over-read the frame.
    if (!R::FunctionParams(g_dropPropFnBase).empty() ||
        !R::FunctionParams(g_spawnKerfuroFn).empty()) {
        UE_LOGE("kerfur_convert: verb signature changed (dropKerfurProp/spawnKerfuro now take params) -- module DISABLED (re-RE the conversion BPs)");
        return;
    }

    // The verbs themselves, at the gate: a name watch fires for a collar variant's own override as for
    // the base's. The name resolves on the game thread (kerfur_form_assembler's tick drives the gate's
    // pending names); a refused registration is a full table, logged by the gate.
    const bool off = sg::WatchName(kRowDropKerfurProp, kWatchTurnOff, &OnVerbPre, &OnVerbPost);
    const bool on  = sg::WatchName(kRowSpawnKerfuro, kWatchTurnOn, &OnVerbPre, &OnVerbPost);
    bool anyInline = false;
    for (InlineTurnOn& k : g_inline) {
        k.propClass = ue_wrap::object_index::ClassByName(k.prop);
        k.npcClass = ue_wrap::object_index::ClassByName(k.npc);
        if (!k.propClass || !k.npcClass)
            UE_LOGW("kerfur_convert: %ls's turn-on is not watched (prop %p, Omega %p) -- it converts on its own machine "
                    "alone", k.prop, k.propClass, k.npcClass);
        anyInline = anyInline || (k.propClass && k.npcClass);
    }
    g_gamerNpcClass = g_inline[0].npcClass;
    if (anyInline && !sg::WatchName(kRowActionOptionIndex, kWatchGamerUse, &OnGamerUsePre, nullptr))
        UE_LOGW("kerfur_convert: the gate took no watch on actionOptionIndex -- the inline turn-ons convert on their own "
                "machine alone");
    if (!off || !on) {
        UE_LOGE("kerfur_convert: the gate refused a verb watch (turn_off=%d turn-on=%d) -- module DISABLED",
                off ? 1 : 0, on ? 1 : 0);
        return;
    }
    // The verb refs and the request latch flip only at this success site; the disabled paths above
    // never reach it, so requests drop there (fail closed; see kerfur_convert_host.h).
    coop::kerfur_convert_host::SetVerbs(g_spawnKerfuroFn, g_killOff);
    UE_LOGI("kerfur_convert: installed (killOff=%d; the conversion is decided at the verb's gate)", g_killOff);
}

bool TryCaptureKerfurPropDestroy(void* actor, coop::element::ElementId dyingEid) {
    namespace KE = coop::kerfur_entity;
    if (!actor) return false;
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return false;
    if (!g_kerfurPropClass || !GT::IsGameThread()) return false;  // the destroy seam is GT; defensive
    void* cls = R::ClassOf(actor);
    if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurPropClass, 1)) return false;
    // Outside a conversion verb this prop form is simply gone, and so is its kerfur's record, or its id
    // is never freed and keeps answering "kerfur" after the registry recycles it. A no-op unless this
    // eid is a tracked kerfur's current form.
    if (!InOwnVerb(actor)) {
        KE::ReleaseKerfurForEid(dyingEid);
        return false;
    }
    // Inside its turn-on: the NPC spawned before this destroy, so its capture decides. Without one the
    // verb made no successor, and this is a plain death the generic relay carries; the return releases
    // the record.
    if (!coop::kerfur_form_assembler::HasCapturedForm(/*wantNpc=*/true)) return false;
    UE_LOGI("kerfur_convert: kerfur prop %p (eid=%u) dies inside its turn-on -- the generic destroy relay "
            "SUPPRESSED, the verb's return converges it", actor, static_cast<unsigned>(dyingEid));
    return true;
}

// The gamer Omega's colour is its tick's alone: each frame it writes HSVToRGB(time alive x 360, 1, 2) into its mesh's
// materials' `color`. A client's copy of an Omega is inert and does not tick, so it stood black, and with the material's
// colour at zero it could read as unseen (VOTV-MP/Multivoid#185). Here the same rainbow is written on such a copy, from
// this machine's clock: the hue is the copy's own, as each machine's own tick would make it. It is painted 30 times a
// second, not each frame (a paint is ~12 us), from a list of the copies refreshed twice a second.
namespace {
struct GamerCopy {
    void* npc;
    int32_t idx;
    void* mesh;
};
std::vector<GamerCopy> g_gamerCopies;
std::chrono::steady_clock::time_point g_gamerScanAt{}, g_gamerPaintAt{};
constexpr auto kGamerScanGap = std::chrono::milliseconds(500);
constexpr auto kGamerPaintGap = std::chrono::microseconds(33333);
}  // namespace

void PaintGamerCopies() {
    auto* s = LoadSession();
    if (!s || !s->running() || s->role() == coop::net::Role::Host || !g_gamerNpcClass) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_gamerPaintAt) return;
    g_gamerPaintAt += kGamerPaintGap;   // on a schedule, so it averages 30 at any frame rate above that
    if (g_gamerPaintAt < now) g_gamerPaintAt = now;
    if (now >= g_gamerScanAt) {
        g_gamerScanAt = now + kGamerScanGap;
        g_gamerCopies.clear();
        for (void* npc : GamerNpcs()) {
            if (ue_wrap::engine::IsActorTickEnabled(npc)) continue;   // a copy that ticks paints itself
            if (void* const mesh = Field<void*>(npc, L"Mesh", nullptr))
                g_gamerCopies.push_back(GamerCopy{npc, R::InternalIndexOf(npc), mesh});
        }
    }
    if (g_gamerCopies.empty()) return;
    // UE4's FLinearColor::HSVToLinearRGB with H in degrees, S 1 and V 2, as the gamer graph calls it.
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const double h = std::fmod(seconds * 360.0, 360.0), c = 2.0;
    const double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    switch (static_cast<int>(h / 60.0) % 6) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    const ue_wrap::FVector rgb{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
    static const R::FName kColor = ue_wrap::fname_utils::StringToFName(L"color");
    for (const GamerCopy& copy : g_gamerCopies) {
        if (!R::IsLiveByIndex(copy.npc, copy.idx)) continue;
        void* const fn = R::FindDispatchFunctionCached(R::ClassOf(copy.mesh), L"SetVectorParameterValueOnMaterials");
        ue_wrap::ParamFrame f(fn);
        if (fn && f.valid() && f.SetRaw(L"ParameterName", &kColor, sizeof(kColor)) &&
            f.Set<ue_wrap::FVector>(L"ParameterValue", rgb))
            ue_wrap::Call(copy.mesh, f);
    }
}

void OnDisconnect() {
    g_open.clear();  // game thread, as the gate callbacks and the destroy seam that read it
    g_gamerCopies.clear();
    g_saidEventRefusal = false;
    coop::kerfur_convert_host::OnDisconnect();
}

}  // namespace coop::kerfur_convert
