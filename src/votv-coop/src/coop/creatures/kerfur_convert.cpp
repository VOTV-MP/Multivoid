// coop/creatures/kerfur_convert.cpp -- the kerfur conversion (NPC to prop and back) decided at its
// verb: a client's gate refuses the verb and asks the host, the host's verb converges at its return.
// The host executor lives in kerfur_convert_host.cpp, the client apply in kerfur_convert_client.cpp.
// See coop/creatures/kerfur_convert.h.

#include "coop/creatures/kerfur_convert.h"

#include "coop/creatures/kerfur_command.h"         // the menu-verb command relay
#include "coop/creatures/kerfur_convert_client.h"  // the wire apply
#include "coop/creatures/kerfur_convert_host.h"    // the converge and the request execution
#include "coop/creatures/kerfur_entity.h"          // the resolved classes, ReleaseKerfurForEid
#include "coop/creatures/kerfur_form_assembler.h"  // HasCapturedForm
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
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

// Resolved engine refs: written by Install on the game thread before the interceptor registers,
// read-only after, including from parallel-anim workers. The kerfur classes are loaded at the main
// menu and live for the process (coop/dev/class_lifetime_probe measures it), so these refs and the
// interceptor hold across a world change.
void* g_kerfurNpcClass  = nullptr;  // kerfurOmega_C (the NPC base; ~20 data-only skin subclasses)
void* g_kerfurPropClass = nullptr;  // prop_kerfurOmega_C (the prop base; skins likewise)
void* g_actionNameFn    = nullptr;  // kerfurOmega_C::actionName (the menu dispatcher -- kerfur_command relay)
int32_t g_nameParamOff  = -1;       // actionName 'name' FString param offset
int32_t g_killOff       = -1;       // kerfurOmega_C::kill bool (the BP's own turn_off guard)
// The base verb declarers, whose signatures the install checks. dropKerfurProp is overridden by the
// collar variants; the host runs the most-derived declarer, looked up on the target's own class.
void* g_dropPropFnBase     = nullptr;  // kerfurOmega_C::dropKerfurProp
void* g_spawnKerfuroFn     = nullptr;  // prop_kerfurOmega_C::spawnKerfuro (sole declarer)

// This lane's gate watches on the two verbs; the form assembler keeps its own on the same names.
constexpr const wchar_t* kTurnOffVerb = L"dropKerfurProp";
constexpr const wchar_t* kTurnOnVerb  = L"spawnKerfuro";
constexpr int kWatchTurnOff = 0x4B430001;  // 'KC' 1
constexpr int kWatchTurnOn  = 0x4B430002;

// An FString param out of a ProcessEvent frame: the 16-byte TArray<wchar_t> {Data, Num, Max}.
// Memory reads only, so worker-safe. Empty on null or an insane length.
std::wstring ReadFStringParam(void* params, int32_t off) {
    if (!params || off < 0) return {};
    auto* base = reinterpret_cast<uint8_t*>(params) + off;
    auto* data = *reinterpret_cast<wchar_t* const*>(base);
    const int32_t num = *reinterpret_cast<const int32_t*>(base + 8);
    if (!data || num <= 1 || num > 256) return {};  // Num includes the terminator
    return std::wstring(data, static_cast<size_t>(num - 1));
}

// The actionName PRE interceptor, for the kerfur_command relay only. Contract: no engine calls,
// no Post, no ProcessEvent re-entry, no registry walks (Snapshot's raw pointers race game-thread
// drains off-thread); memory reads and the atomic session load only.
bool OnKerfurActionNamePre(void* self, void* params) {
    if (!self || !params || g_nameParamOff < 0) return false;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return false;  // SP untouched
    const std::wstring name = ReadFStringParam(params, g_nameParamOff);
    if (name != L"turn_off") {
        // The state-changing radial verbs (follow, idle, patrol, fix_servers, get_reports,
        // fix_transformers) go to the host-authoritative command relay, which records the action
        // and returns true (cancel) for a relayed verb and false for an unrelayed one.
        const bool isClient = s->role() == coop::net::Role::Client;
        return coop::kerfur_command::TryRecordMenuCommand(self, name, isClient);
    }
    // turn_off runs on into dropKerfurProp, which this lane's gate watch decides.
    return false;
}

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

sg::Verdict OnVerbPre(const sg::Call& c) {
    auto* s = LoadSession();
    if (!s || !s->running()) return sg::Verdict::Run;
    const bool toProp = (c.tag == kWatchTurnOff);
    if (s->role() == coop::net::Role::Client) {
        RefuseOnClient(s, c, toProp);
        return sg::Verdict::Cancel;
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
        coop::kerfur_convert_host::ConvergeAtReturn(o.object, o.idx, o.eid, o.toProp, o.requestSlot);
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

    if (!g_actionNameFn) {
        g_actionNameFn = R::FindFunction(g_kerfurNpcClass, L"actionName");
        if (g_actionNameFn) {
            g_nameParamOff = R::FindParamOffset(g_actionNameFn, L"name");
        }
    }
    if (!g_dropPropFnBase) g_dropPropFnBase = R::FindFunction(g_kerfurNpcClass, kTurnOffVerb);
    if (!g_spawnKerfuroFn) g_spawnKerfuroFn = R::FindFunction(g_kerfurPropClass, kTurnOnVerb);
    if (g_killOff < 0)     g_killOff = R::FindPropertyOffset(g_kerfurNpcClass, L"kill");

    coop::kerfur_convert_host::SetClasses(g_kerfurNpcClass, g_kerfurPropClass);

    if (!g_actionNameFn || g_nameParamOff < 0 || !g_dropPropFnBase || !g_spawnKerfuroFn) {
        UE_LOGE("kerfur_convert: partial resolve (actionName=%p nameOff=%d drop=%p spawn=%p) -- module "
                "DISABLED (a recooked kerfur Blueprint)",
                g_actionNameFn, g_nameParamOff, g_dropPropFnBase, g_spawnKerfuroFn);
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

    // The relay's interceptor on the menu dispatcher.
    if (!GT::RegisterInterceptor(g_actionNameFn, &OnKerfurActionNamePre)) {
        UE_LOGE("kerfur_convert: RegisterInterceptor(actionName) failed (the interceptor table is full) "
                "-- module DISABLED");
        return;
    }
    // The verbs themselves, at the gate: a name watch fires for a collar variant's own override as for
    // the base's. The name resolves on the game thread (kerfur_form_assembler's tick drives the gate's
    // pending names); a refused registration is a full table, logged by the gate.
    const bool off = sg::WatchName(kTurnOffVerb, kWatchTurnOff, &OnVerbPre, &OnVerbPost);
    const bool on  = sg::WatchName(kTurnOnVerb,  kWatchTurnOn,  &OnVerbPre, &OnVerbPost);
    if (!off || !on) {
        UE_LOGE("kerfur_convert: the gate refused a verb watch (turn_off=%d turn-on=%d) -- module DISABLED",
                off ? 1 : 0, on ? 1 : 0);
        return;
    }
    // The verb refs and the request latch flip only at this success site; the disabled paths above
    // never reach it, so requests drop there (fail closed; see kerfur_convert_host.h).
    coop::kerfur_convert_host::SetVerbs(g_spawnKerfuroFn, g_killOff);
    UE_LOGI("kerfur_convert: installed (actionName nameOff=%d, killOff=%d; the conversion is decided at the "
            "verb's gate)", g_nameParamOff, g_killOff);
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

void OnDisconnect() {
    g_open.clear();  // game thread, as the gate callbacks and the destroy seam that read it
    g_saidEventRefusal = false;
    coop::kerfur_convert_host::OnDisconnect();
}

}  // namespace coop::kerfur_convert
