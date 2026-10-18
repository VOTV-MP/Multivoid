// coop/dev/pi3_observer.cpp -- see coop/dev/pi3_observer.h.

#include "coop/dev/pi3_observer.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // FindParamOffset
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace coop::dev::pi3_observer {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace PL = coop::players;

// ---- the fact rows ----

UE_FACT_FN(kRowPi3LoadLevel, "dev.pi3_drill", L"lib_C", L"loadLevel", Declared);
UE_FACT_FN(kRowPi3End, "dev.pi3_drill", L"lib_C", L"end", Declared);
UE_FACT_FN(kRowPi3SetSettings, "dev.pi3_drill", L"lib_C", L"setSettings", Declared);
UE_FACT_FN(kRowPi3SaveSlotSave, "dev.pi3_drill", L"saveSlot_C", L"save", Declared);
UE_FACT_FN(kRowPi3SaveMainSave, "dev.pi3_drill", L"save_main_C", L"save", Declared);
UE_FACT_FN(kRowPi3GamemodeSave, "dev.pi3_drill", L"mainGamemode_C", L"save", Declared);
UE_FACT_FN(kRowPi3Rewind, "dev.pi3_drill", L"daynightCycle_C", L"rewind", Declared);
UE_FACT_FN(kRowPi3Punch, "dev.pi3_drill", L"mainPlayer_C", L"punch", Declared);
UE_FACT_FN(kRowPi3QuitGame, "dev.pi3_drill", L"KismetSystemLibrary", L"QuitGame", Declared);
UE_FACT_FN(kRowPi3SetControlRotation, "dev.pi3_drill", L"Controller", L"SetControlRotation", Declared);
UE_FACT_FN(kRowPi3PlaySound2D, "dev.pi3_drill", L"GameplayStatics", L"PlaySound2D", Declared);
UE_FACT_FN(kRowPi3AddToViewport, "dev.pi3_drill", L"UserWidget", L"AddToViewport", Declared);
UE_FACT_FN(kRowPi3Spawn, "dev.pi3_drill", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
UE_FACT_FN(kRowPi3Finish, "dev.pi3_drill", L"GameplayStatics", L"FinishSpawningActor", Declared);

// The six row stubs of PI-2b's names (rows 1-5 and 11), watched here under this file's own tags.
UE_FACT_FN(kRowPi3ContactWispW, "dev.pi3_drill", L"wisp_w_C",
           L"BndEvt__wisp_red_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPi3ContactWispO, "dev.pi3_drill", L"wisp_o_C",
           L"BndEvt__wisp_p_Sphere1_K2Node_ComponentBoundEvent_3_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPi3ContactWispP, "dev.pi3_drill", L"wisp_p_C",
           L"BndEvt__wisp_p_Sphere1_K2Node_ComponentBoundEvent_3_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPi3ContactKelura, "dev.pi3_drill", L"kelura_C",
           L"BndEvt__kelura_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPi3ContactOrborb, "dev.pi3_drill", L"npc_orborb_C",
           L"BndEvt__blackBall_Sphere1_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPi3ContactGrunt, "dev.pi3_drill", L"prop_grunt_C",
           L"BndEvt__prop_grunt_damageSphere_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);

// ---- the verbs ----

// The script verbs come first, in the order of kScripts; the natives follow.
enum Verb : int {
    kLoadLevel, kEnd, kSetSettings, kSaveSlot, kSaveMain, kGamemodeSave, kRewind, kPunch,
    kScriptVerbs,
    kQuit = kScriptVerbs, kSetRot, kPlaySound, kAddViewport, kGib, kBirth,
    kVerbCount
};
const char* const kVerbNames[kVerbCount] = {
    "lib_C::loadLevel", "lib_C::end", "lib_C::setSettings", "saveSlot_C::save", "save_main_C::save",
    "mainGamemode_C::save", "daynightCycle_C::rewind", "mainPlayer_C::punch",
    "QuitGame", "SetControlRotation", "PlaySound2D", "AddToViewport", "gib", "birth",
};
// The verbs cancelled when attributed; the others are recorded and run.
bool Cancels(int verb) { return verb <= kGamemodeSave || verb == kQuit; }

const ue_wrap::facts::Function* const kScripts[kScriptVerbs] = {
    &kRowPi3LoadLevel, &kRowPi3End, &kRowPi3SetSettings, &kRowPi3SaveSlotSave, &kRowPi3SaveMainSave,
    &kRowPi3GamemodeSave, &kRowPi3Rewind, &kRowPi3Punch,
};
constexpr int kTagVerb = 0x50493300;  // 'PI3' and the verb

constexpr int kContactCount = 6;
const ue_wrap::facts::Function* const kContacts[kContactCount] = {
    &kRowPi3ContactWispW, &kRowPi3ContactWispO, &kRowPi3ContactWispP,
    &kRowPi3ContactKelura, &kRowPi3ContactOrborb, &kRowPi3ContactGrunt,
};
constexpr int kTagContact = 0x50493400;  // 'PI3' and the contact

sg::Verdict OnVerbPre(const sg::Call& call);
sg::Verdict OnContactPre(const sg::Call& call);
sg::Verdict OnQuitPre(void* context, void* source, const uint8_t* parms);
void OnSetRotationPost(void* context, void* source, void* result);
void OnPlaySoundPost(void* context, void* source, void* result);
void OnAddViewportPost(void* context, void* source, void* result);
void OnSpawnPost(void* context, void* source, void* result);
void OnFinishPost(void* context, void* source, void* result);

// One native hook: a pre hook that cancels (QuitGame) or a post hook that records.
struct Hook {
    const ue_wrap::facts::Function* row;
    Verb                            verb;
    UH::PreNativeCallback           pre;
    UH::PostNativeCallback          post;
    void*                           fn;
    bool                            done;
    bool                            failed;
};
constexpr int kHookCount = 6;
Hook g_hooks[kHookCount] = {
    {&kRowPi3QuitGame, kQuit, &OnQuitPre, nullptr, nullptr, false, false},
    {&kRowPi3SetControlRotation, kSetRot, nullptr, &OnSetRotationPost, nullptr, false, false},
    {&kRowPi3PlaySound2D, kPlaySound, nullptr, &OnPlaySoundPost, nullptr, false, false},
    {&kRowPi3AddToViewport, kAddViewport, nullptr, &OnAddViewportPost, nullptr, false, false},
    {&kRowPi3Spawn, kGib, nullptr, &OnSpawnPost, nullptr, false, false},
    {&kRowPi3Finish, kBirth, nullptr, &OnFinishPost, nullptr, false, false},
};

// ---- the state ----

struct Rec {
    int      count;
    int      cancelled;
    uint64_t firstAt;
};
Rec      g_rec[kVerbCount] = {};
Seen     g_report[kVerbCount] = {};
bool     g_installed = false;
bool     g_liveSaid = false;
bool     g_armed = false;
Mode     g_mode = Mode::Spawner;
int      g_nClasses = 0;
// The armed classes by name, read at Arm. A class object is perishable (unloaded, loaded again at a new address) and
// the engine never frees a name, so a caller's class chain is matched against these and no class pointer is held.
R::FName g_classFNames[2] = {};  // the first g_nClasses are the armed classes' names
// The spawned site, slot-checked: a site that destroyed itself inside the window is not matched by an object born
// at its address after it.
ue_wrap::CachedObjRef g_site;
bool     g_contactArmed[kContactCount] = {};
bool     g_changed = false;  // a record moved since TakeChanged
uint64_t g_armAt = 0;
uint64_t g_anchorAt = 0;  // the first contact, once there is one
int      g_puppetContacts = 0;
int      g_ownContacts = 0;
int      g_hookTries = 0;
uint64_t g_hookTryAt = 0;

// The tick count (the watch sweep runs at most once a tick while pending).
uint64_t g_tickNo = 0;

// The name watches' state, swept once and kept: every watch settled, and the live ones, in the Live sense (a
// contact watch only for the armed classes once armed). A settled sweep is asked again only when the class set
// moved or Arm/Disarm changed which watches are required. ContactWatchLive does not read it.
struct Watches {
    bool     valid;
    bool     pending;
    bool     live;
    uint64_t ver;
    uint64_t tick;
};
Watches g_ws = {};

// A hook whose row has not resolved is looked for again twice a second, ten times, and then called broken.
constexpr int      kHookTries = 10;
constexpr uint64_t kHookRetryMs = 500;

// Each contact stub's OtherActor parameter, found by name at its first call; -1 when the stub has none.
constexpr int32_t kUnread = INT32_MIN;
int32_t g_actorOff[kContactCount] = {kUnread, kUnread, kUnread, kUnread, kUnread, kUnread};

uint64_t Now() { return ::GetTickCount64(); }

void Note(int verb, bool cancelled) {
    g_changed = true;
    Rec& r = g_rec[verb];
    if (r.count++ == 0) r.firstAt = Now();
    if (cancelled) ++r.cancelled;
}

void Refresh() {
    const uint64_t ver = OI::ClassSetVersion();
    if (g_ws.valid && g_ws.ver == ver && (!g_ws.pending || g_ws.tick == g_tickNo)) return;
    bool pending = false, live = true;
    for (int i = 0; i < kScriptVerbs; ++i) {
        if (!sg::NameWatchSettled(*kScripts[i], kTagVerb + i)) {
            pending = true;
            live = false;
        } else if (live && !sg::NameWatchLive(*kScripts[i], kTagVerb + i)) {
            live = false;
        }
    }
    for (int i = 0; i < kContactCount; ++i) {
        const bool settled = sg::NameWatchSettled(*kContacts[i], kTagContact + i);
        if (!settled) pending = true;
        if (g_armed && !g_contactArmed[i]) continue;
        if (!settled || !sg::NameWatchLive(*kContacts[i], kTagContact + i)) live = false;
    }
    g_ws = Watches{true, pending, live, ver, g_tickNo};
}

// Does the chain from `cls` up hold a class named like an armed one. The names are FNames (an index compare, no
// string), so a class unloaded and loaded again at a new address is still the armed one.
bool ChainHasArmedName(void* cls) {
    constexpr int kHops = 16;
    for (int hop = 0; cls && hop < kHops; ++hop, cls = R::SuperStructOf(cls)) {
        const R::FName& name = R::NameOf(cls);
        for (int k = 0; k < g_nClasses; ++k)
            if (name.ComparisonIndex == g_classFNames[k].ComparisonIndex && name.Number == g_classFNames[k].Number)
                return true;
    }
    return false;
}

// Does a call whose caller is `obj` belong to the arm: armed, and the site itself once it is named, else any
// creature of an armed class.
bool Attributed(void* obj) {
    if (!g_armed || !obj) return false;
    if (g_site.Raw()) return g_site.Alive() && obj == g_site.Raw();
    return ChainHasArmedName(R::ClassOf(obj));
}

bool IsPlayerController(void* obj) {
    static void* s_pc = nullptr;  // an engine class: listed at boot, never unlisted
    if (!s_pc) s_pc = OI::ClassByName(P::name::PlayerControllerClassName);
    return obj && s_pc && R::IsDescendantOfAny(R::ClassOf(obj), &s_pc, 1);
}

// ---- the callbacks ----

sg::Verdict OnVerbPre(const sg::Call& call) {
    if (!g_armed) return sg::Verdict::Run;
    const int verb = call.tag - kTagVerb;
    if (verb < 0 || verb >= kScriptVerbs || !Attributed(call.callerObject)) return sg::Verdict::Run;
    if (verb == kPunch && call.object != PL::Registry::Get().Local()) return sg::Verdict::Run;
    const bool cancel = Cancels(verb);
    Note(verb, cancel);
    return cancel ? sg::Verdict::Cancel : sg::Verdict::Run;
}

sg::Verdict OnContactPre(const sg::Call& call) {
    if (!g_armed) return sg::Verdict::Run;
    const int index = call.tag - kTagContact;
    if (index < 0 || index >= kContactCount || !g_contactArmed[index] || !Attributed(call.object))
        return sg::Verdict::Run;
    int32_t& off = g_actorOff[index];
    if (off == kUnread) {
        off = R::FindParamOffset(call.function, L"OtherActor");
        if (off < 0) UE_LOGI("[PI-3] BROKEN contact-offset %ls", kContacts[index]->Class());
    }
    if (off < 0 || !call.locals) return sg::Verdict::Run;
    void* const other = *reinterpret_cast<void* const*>(call.locals + off);
    if (!other) return sg::Verdict::Run;
    const bool puppet = PL::Registry::Get().IsPuppetBody(other);
    const bool own = !puppet && PL::Registry::Get().IsLocal(other);
    if (!puppet && !own) return sg::Verdict::Run;
    if (g_mode != Mode::Still && !g_anchorAt) g_anchorAt = Now();
    g_changed = true;
    if (puppet) {
        ++g_puppetContacts;
        return sg::Verdict::Run;
    }
    ++g_ownContacts;
    return g_mode == Mode::Still ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// The native hooks attribute by FFrame::Object, the Blueprint whose bytecode made the call. A pre hook and a name
// watch forward an off-thread call untouched (ufunction_hook.h: "Off the game thread it forwards untouched"). Each
// post hook returns first off the game thread, as host_spawn_watcher's OnSpawnPost does
// (coop/props/host_spawn_watcher.cpp): these write the observer's plain statics.
sg::Verdict OnQuitPre(void*, void* source, const uint8_t*) {
    if (!Attributed(source)) return sg::Verdict::Run;
    Note(kQuit, true);
    return sg::Verdict::Cancel;
}

void OnSetRotationPost(void* context, void* source, void*) {
    if (!GT::IsGameThread()) return;
    if (Attributed(source) && IsPlayerController(context)) Note(kSetRot, false);
}

void OnPlaySoundPost(void*, void* source, void*) {
    if (!GT::IsGameThread()) return;
    if (Attributed(source)) Note(kPlaySound, false);
}

void OnAddViewportPost(void*, void* source, void*) {
    if (!GT::IsGameThread()) return;
    if (Attributed(source)) Note(kAddViewport, false);
}

// A gib: a deferred spawn whose result is a prop.
void OnSpawnPost(void*, void* source, void* result) {
    if (!GT::IsGameThread()) return;
    if (!result || !Attributed(source)) return;
    void* prop = OI::ClassByName(P::name::PropClass);
    if (prop && R::IsDescendantOfAny(R::ClassOf(result), &prop, 1)) Note(kGib, false);
}

// A birth: a spawn finished whose actor descends from an armed class. The class's BeginPlay runs inside the finish,
// so every verb of that birth is recorded by the time this post fires, or never will be. The result is the finished
// actor; the match is by the class names, as a caller's is, so a mirror (no bound site) is counted too.
void OnFinishPost(void*, void*, void* result) {
    if (!GT::IsGameThread()) return;
    if (g_armed && result && ChainHasArmedName(R::ClassOf(result))) Note(kBirth, false);
}

// ---- the installs ----

void ArmHooks(bool on) {
    for (const Hook& h : g_hooks) {
        if (!h.done) continue;
        if (h.pre) UH::SetArmed(h.fn, h.pre, on);
        else UH::SetArmed(h.fn, h.post, on);
    }
}

void TryHooks(bool final) {
    for (Hook& h : g_hooks) {
        if (h.done || h.failed) continue;
        const ue_wrap::facts::Resolved r = ue_wrap::facts::Resolve(*h.row);
        if (!r) {
            if (final) {
                h.failed = true;
                UE_LOGI("[PI-3] BROKEN hook %s", kVerbNames[h.verb]);
            }
            continue;
        }
        const bool ok = h.pre ? UH::InstallPreHook(r, h.pre, /*armed=*/false)
                              : UH::InstallPostHook(r, h.post, /*armed=*/false);
        if (ok) {
            h.fn = r.Fn();
            h.done = true;
        } else {
            h.failed = true;
            UE_LOGI("[PI-3] BROKEN hook %s", kVerbNames[h.verb]);
        }
    }
}

}  // namespace

void Install() {
    if (g_installed) return;
    g_installed = true;
    for (int i = 0; i < kScriptVerbs; ++i)
        if (!sg::WatchName(*kScripts[i], kTagVerb + i, &OnVerbPre, nullptr))
            UE_LOGI("[PI-3] BROKEN hook %ls::%ls", kScripts[i]->Class(), kScripts[i]->Name());
    for (int i = 0; i < kContactCount; ++i)
        if (!sg::WatchName(*kContacts[i], kTagContact + i, &OnContactPre, nullptr))
            UE_LOGI("[PI-3] BROKEN hook %ls::contact", kContacts[i]->Class());
    TryHooks(/*final=*/false);
}

void Tick() {
    if (!g_installed) return;
    ++g_tickNo;
    bool hooksPending = false;
    for (const Hook& h : g_hooks) hooksPending = hooksPending || (!h.done && !h.failed);
    if (hooksPending && Now() - g_hookTryAt >= kHookRetryMs) {
        g_hookTryAt = Now();
        TryHooks(++g_hookTries >= kHookTries);
        if (g_armed) ArmHooks(true);
    }
    if (g_armed || !g_ws.valid || g_ws.pending) sg::ResolvePendingNames();
    Refresh();
    if (!g_liveSaid && Live()) {
        g_liveSaid = true;
        UE_LOGI("[PI-3] observer live hooks=%d watches=%d", kHookCount, kScriptVerbs + kContactCount);
    }
}

bool Live() {
    if (!g_installed) return false;
    for (const Hook& h : g_hooks)
        if (!h.done) return false;
    Refresh();
    return g_ws.live;
}

bool ContactWatchLive() {
    if (!g_armed) return true;
    for (int i = 0; i < kContactCount; ++i)
        if (g_contactArmed[i] && !sg::NameWatchLive(*kContacts[i], kTagContact + i)) return false;
    return true;
}

bool FirstPending(char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    for (const Hook& h : g_hooks)
        if (!h.done) {
            std::snprintf(out, cap, "hook %s", kVerbNames[h.verb]);
            return true;
        }
    for (int i = 0; i < kScriptVerbs; ++i)
        if (!sg::NameWatchLive(*kScripts[i], kTagVerb + i)) {
            std::snprintf(out, cap, "%ls::%ls", kScripts[i]->Class(), kScripts[i]->Name());
            return true;
        }
    for (int i = 0; i < kContactCount; ++i)
        if ((!g_armed || g_contactArmed[i]) && !sg::NameWatchLive(*kContacts[i], kTagContact + i)) {
            std::snprintf(out, cap, "%ls::%ls", kContacts[i]->Class(), kContacts[i]->Name());
            return true;
        }
    return false;
}

void Arm(void* const* classes, int n, Mode mode) {
    g_nClasses = 0;  // a null class is skipped, never stored
    for (int i = 0; i < n && g_nClasses < 2; ++i)
        if (classes[i]) g_classFNames[g_nClasses++] = R::NameOf(classes[i]);
    for (int i = 0; i < kContactCount; ++i) {
        g_contactArmed[i] = false;
        for (int k = 0; k < g_nClasses; ++k)
            g_contactArmed[i] = g_contactArmed[i] || R::NameEquals(g_classFNames[k], kContacts[i]->Class());
    }
    for (Rec& r : g_rec) r = Rec{};
    g_changed = false;
    g_mode = mode;
    g_site.Reset();
    g_puppetContacts = g_ownContacts = 0;
    g_armAt = Now();
    g_anchorAt = mode == Mode::Still ? g_armAt : 0;
    g_armed = true;
    ArmHooks(true);
    g_ws.valid = false;
    Refresh();
}

void Bind(void* site) { g_site.Set(site); }

void Disarm() {
    g_armed = false;
    g_site.Reset();
    ArmHooks(false);
    g_ws.valid = false;
}

const Seen* Report(int& n) {
    n = 0;
    const uint64_t anchor = g_anchorAt ? g_anchorAt : g_armAt;
    for (int v = 0; v < kVerbCount; ++v) {
        const Rec& r = g_rec[v];
        if (r.count == 0) continue;
        g_report[n++] = Seen{kVerbNames[v], r.count, r.cancelled,
                             static_cast<int>(static_cast<int64_t>(r.firstAt) - static_cast<int64_t>(anchor))};
    }
    return g_report;
}

bool TakeChanged() {
    const bool changed = g_changed;
    g_changed = false;
    return changed;
}

int PuppetContacts() { return g_puppetContacts; }

int OwnContacts() { return g_ownContacts; }

int Births() { return g_rec[kBirth].count; }

}  // namespace coop::dev::pi3_observer
