// coop/dev/keypad_probe.cpp -- see coop/dev/keypad_probe.h.

#include "coop/dev/keypad_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/call.h"
#include "ue_wrap/door.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/passwordlock.h"
#include "ue_wrap/reflection.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::dev::keypad_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PL = ue_wrap::passwordlock;

bool ProbeEnabled() {
    static const bool s_enabled = coop::config::IsIniKeyTrue("keypad_probe");
    return s_enabled;
}

// --- observed-verb confirm (a FIRED line = that edge IS ProcessEvent-observable) ---
struct WatchedFn { void* fn = nullptr; const char* name = nullptr; };
std::array<WatchedFn, 6> g_watched{};
size_t g_watchedCount = 0;

void OnKeypadVerb(void* self, void* function, void* /*params*/) {
    if (!ProbeEnabled()) return;
    for (size_t i = 0; i < g_watchedCount; ++i) {
        if (g_watched[i].fn != function) continue;
        UE_LOGI("[keypad_probe] VERB FIRED: %s (self=%p) -- ProcessEvent-OBSERVABLE", g_watched[i].name, self);
        return;
    }
}

void RegisterOn(void* cls, const wchar_t* fnName, const char* label) {
    if (!cls) return;
    void* fn = R::FindFunction(cls, fnName);
    if (!fn) { UE_LOGW("[keypad_probe] '%ls' not found", fnName); return; }
    for (size_t i = 0; i < g_watchedCount; ++i) if (g_watched[i].fn == fn) return;
    if (!GT::RegisterPostObserver(fn, &OnKeypadVerb)) return;
    if (g_watchedCount < g_watched.size()) g_watched[g_watchedCount++] = WatchedFn{fn, label};
}

bool     g_installed = false;
void*    g_cls       = nullptr;
void*    g_inputNumFn = nullptr;  // inputNumber(int32 Num)
int32_t  g_numOff    = -1;        // Num         @0x0378
int32_t  g_isAccOff  = -1;        // isAcc       @0x037C
int32_t  g_isDenyOff = -1;        // isDeny      @0x037D
int32_t  g_inPwOff   = -1;        // inPassword  @0x0380 (FString)
int32_t  g_pwOff     = -1;        // password    @0x0350 (FString)
int32_t  g_focusOff  = -1;        // isFocused   @0x0391
int32_t  g_doorOff   = -1;        // door        @0x0338 (Adoor_C* the keypad gates)
uint64_t g_tick      = 0;
bool     g_testDone  = false;

std::unordered_map<void*, uint32_t> g_lastState;  // ptr -> packed (Num<<2 | isAcc<<1 | isDeny)

template <typename T> T ReadAt(void* obj, int32_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

std::wstring ReadFString(void* obj, int32_t off) {
    if (!obj || off < 0) return std::wstring();
    const R::FString& s = *reinterpret_cast<const R::FString*>(reinterpret_cast<uint8_t*>(obj) + off);
    if (!s.Data || s.Num <= 1 || s.Num > 4096) return std::wstring();
    return std::wstring(s.Data, s.Data + (s.Num - 1));  // Num counts the null terminator
}

}  // namespace

void Install() {
    if (!ProbeEnabled() || g_installed) return;
    g_cls = R::FindClass(L"passwordLock_C");
    if (!g_cls) return;  // BP not loaded yet -- retry
    PL::EnsureResolved();

    // Confirm these are BP-internal (NONE should fire on a real code entry).
    RegisterOn(g_cls, L"inputNumber",     "passwordLock.inputNumber");
    RegisterOn(g_cls, L"Open",            "passwordLock.Open");
    RegisterOn(g_cls, L"falseEnterEvent", "passwordLock.falseEnterEvent");
    RegisterOn(g_cls, L"player_use",      "passwordLock.player_use");

    g_inputNumFn = R::FindFunction(g_cls, L"inputNumber");
    g_numOff    = R::FindPropertyOffset(g_cls, L"Num");
    g_isAccOff  = R::FindPropertyOffset(g_cls, L"isAcc");
    g_isDenyOff = R::FindPropertyOffset(g_cls, L"isDeny");
    g_inPwOff   = R::FindPropertyOffset(g_cls, L"inPassword");
    g_pwOff     = R::FindPropertyOffset(g_cls, L"password");
    g_focusOff  = R::FindPropertyOffset(g_cls, L"isFocused");
    g_doorOff   = R::FindPropertyOffset(g_cls, L"door");
    ue_wrap::door::EnsureResolved();  // so TryReadOpen can read the gated door's isOpened
    g_installed = true;
    UE_LOGI("[keypad_probe] installed %zu observers; inputNumber=%p Num@0x%X isAcc@0x%X isDeny@0x%X "
            "inPassword@0x%X password@0x%X isFocused@0x%X. ENTER A REAL CODE -- the passive poll logs "
            "the inPassword/Num/isAcc transitions; a 'VERB FIRED' line would mean that verb is observable.",
            g_watchedCount, g_inputNumFn, g_numOff, g_isAccOff, g_isDenyOff, g_inPwOff, g_pwOff, g_focusOff);
}

void Tick() {
    if (!ProbeEnabled() || !g_installed) return;

    // --- passive poll: log state CHANGES on focused/in-progress keypads (hands-on) ---
    if ((g_tick % 24) == 0 && g_numOff >= 0 && g_isAccOff >= 0) {
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !PL::IsPasswordLock(o) || !R::IsLive(o)) continue;
            const bool foc  = (g_focusOff >= 0) && ReadAt<bool>(o, g_focusOff);
            const int  num  = (int)ReadAt<uint8_t>(o, g_numOff);   // Num is int32; low byte is enough for <10 digits
            const int  acc  = (int)ReadAt<uint8_t>(o, g_isAccOff);
            const int  deny = (g_isDenyOff >= 0) ? (int)ReadAt<uint8_t>(o, g_isDenyOff) : 0;
            if (!foc && num == 0 && acc == 0) continue;  // idle keypad -- skip
            const uint32_t packed = (uint32_t)(ReadAt<int32_t>(o, g_numOff) << 2) | (uint32_t)(acc << 1) | (uint32_t)deny;
            auto it = g_lastState.find(o);
            if (it != g_lastState.end() && it->second == packed) continue;  // unchanged
            g_lastState[o] = packed;
            UE_LOGI("[keypad_probe] STATE key='%ls' focused=%d Num=%d isAcc=%d isDeny=%d inPassword='%ls' password='%ls'",
                    PL::GetKeyString(o).c_str(), foc ? 1 : 0, ReadAt<int32_t>(o, g_numOff), acc, deny,
                    ReadFString(o, g_inPwOff).c_str(), ReadFString(o, g_pwOff).c_str());
        }
    }

    // --- one-shot SYNTHETIC inputNumber sequence (AUTONOMOUS only) ---
    // Gated behind a SEPARATE flag so a hands-on `keypad_probe=1` is PURELY PASSIVE and
    // never types into the keypad the user is using (the synthetic typing would corrupt a
    // real entry / trip falseEnterEvent). Set `keypad_synth=1` only for an autonomous run.
    static const bool sSynth = coop::config::IsIniKeyTrue("keypad_synth");
    if (!sSynth) { g_tick++; return; }
    if (g_testDone) { g_tick++; return; }
    if (g_tick++ < 360) return;  // let the world stream in
    g_testDone = true;

    if (!g_inputNumFn) { UE_LOGW("[keypad_probe] inputNumber not resolved -- synthetic test skipped"); return; }
    // Pick a REAL placed keypad: keyed (Key != None) AND door-wired (door != null). The prior
    // run grabbed an unkeyed/door-null template, which confounds the accept test (Open's success
    // path drives the gated door). Fall back to any keyed lock, then any lock.
    auto doorOf = [&](void* o) -> void* { return (g_doorOff >= 0) ? ReadAt<void*>(o, g_doorOff) : nullptr; };
    auto keyedOf = [&](void* o) { std::wstring k = PL::GetKeyString(o); return !k.empty() && k != L"None"; };
    void* lock = nullptr; void* lockKeyedOnly = nullptr; void* lockAny = nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !PL::IsPasswordLock(o) || !R::IsLive(o)) continue;
        if (!lockAny) lockAny = o;
        if (keyedOf(o)) {
            if (!lockKeyedOnly) lockKeyedOnly = o;
            if (doorOf(o)) { lock = o; break; }
        }
    }
    if (!lock) lock = lockKeyedOnly ? lockKeyedOnly : lockAny;
    if (!lock) { UE_LOGW("[keypad_probe] no live passwordLock_C found -- synthetic test skipped"); return; }

    const std::wstring pw = ReadFString(lock, g_pwOff);
    void* doorPtr = doorOf(lock);
    bool doorOpenBefore = false;
    const bool haveDoor = doorPtr && ue_wrap::door::TryReadOpen(doorPtr, doorOpenBefore);
    const bool accB = ReadAt<bool>(lock, g_isAccOff);
    UE_LOGI("[keypad_probe] SYNTH TEST on key='%ls' door=%p(open=%s): password='%ls' isAcc=%d inPassword='%ls' -- typing it digit-by-digit...",
            PL::GetKeyString(lock).c_str(), doorPtr, haveDoor ? (doorOpenBefore ? "1" : "0") : "n/a",
            pw.c_str(), accB ? 1 : 0, ReadFString(lock, g_inPwOff).c_str());
    if (pw.empty()) { UE_LOGW("[keypad_probe] password empty/unreadable -- typing 1,2,3,4 instead"); }

    // ===== RECEIVER-MECHANISM HUNT (round 2): FOCUS-FIRST. =====
    // Round 1 proved Open/open2/SetActive are inert. But it called them WITHOUT focus context --
    // the real flow is focusOn() (sets entering/isFocused) -> inputNumber(digits) -> ENTER handler.
    // The accept handler likely bails unless `entering`/`isFocused` is set. Here we focusOn() FIRST,
    // THEN type, THEN try the evaluate/submit handlers (isButtonUsed/processKeys/Open), reading
    // isAcc + the gated door + entering/isFocused after each. If one accepts natively (isAcc 0->1,
    // door 0->1) the receiver replays digits IN A FOCUS CONTEXT then calls it (native LED + door).
    // Else -> direct-write isAcc/isDeny (LED repaint = hands-on check; door is the door channel's job).
    const int32_t enterOff = R::FindPropertyOffset(g_cls, L"entering");
    const int32_t actOff   = R::FindPropertyOffset(g_cls, L"Active");
    auto snap = [&](const char* label) {
        const bool acc = ReadAt<bool>(lock, g_isAccOff);
        const int deny  = (g_isDenyOff >= 0) ? (int)ReadAt<uint8_t>(lock, g_isDenyOff) : -1;
        const int ent   = (enterOff   >= 0) ? (int)ReadAt<uint8_t>(lock, enterOff)   : -1;
        const int foc   = (g_focusOff >= 0) ? (int)ReadAt<uint8_t>(lock, g_focusOff) : -1;
        const int act   = (actOff     >= 0) ? (int)ReadAt<uint8_t>(lock, actOff)     : -1;
        bool dop = false; const bool hd = doorPtr && ue_wrap::door::TryReadOpen(doorPtr, dop);
        UE_LOGI("[keypad_probe]   after %s: isAcc=%d isDeny=%d entering=%d isFocused=%d Active=%d door=%s inPassword='%ls'",
                label, acc ? 1 : 0, deny, ent, foc, act, hd ? (dop ? "1" : "0") : "n/a", ReadFString(lock, g_inPwOff).c_str());
        return acc;
    };
    auto callNoArg = [&](const wchar_t* fnName) {
        void* fn = R::FindFunction(g_cls, fnName);
        if (!fn) { UE_LOGI("[keypad_probe]   (%ls not found)", fnName); return; }
        ue_wrap::ParamFrame f(fn); if (f.valid()) ue_wrap::Call(lock, f);  // out-param fns: block alloc'd, out ignored
    };
    auto callBool = [&](const wchar_t* fnName, const wchar_t* param, bool v) {
        void* fn = R::FindFunction(g_cls, fnName);
        if (!fn) { UE_LOGI("[keypad_probe]   (%ls not found)", fnName); return; }
        ue_wrap::ParamFrame f(fn); if (f.valid()) { f.Set<bool>(param, v); ue_wrap::Call(lock, f); }
    };

    snap("initial");
    callNoArg(L"focusOn");                 snap("focusOn()");
    const std::wstring seq = pw.empty() ? std::wstring(L"1234") : pw;
    for (size_t d = 0; d < seq.size() && d < 16; ++d) {
        wchar_t ch = seq[d];
        if (ch < L'0' || ch > L'9') { UE_LOGI("[keypad_probe]   skip non-digit '%lc'", ch); continue; }
        int32_t digit = ch - L'0';
        ue_wrap::ParamFrame f(g_inputNumFn);
        if (f.valid()) { f.Set<int32_t>(L"Num", digit); ue_wrap::Call(lock, f); }
    }
    const bool accType = snap("type(focused)");
    callNoArg(L"isButtonUsed");            const bool accBtn   = snap("isButtonUsed()");
    callNoArg(L"processKeys");             const bool accProc  = snap("processKeys()");
    callBool(L"Open", L"Active", true);    const bool accOpen  = snap("Open(true)");

    // Last resort: DIRECT FIELD WRITE of isAcc (what the receiver falls back to if no verb accepts).
    if (g_isAccOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(lock) + g_isAccOff) = true;
    if (g_isDenyOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(lock) + g_isDenyOff) = false;
    const bool accWrite = snap("DIRECT-WRITE isAcc=1");
    callNoArg(L"unfocus");  // clean up the focus we established

    const char* verb = accType ? "type(focused) auto-accepted" : accBtn ? "isButtonUsed()" : accProc ? "processKeys()"
                     : accOpen ? "Open(true) [focused]" : "NONE (no verb accepts -> direct-write isAcc)";
    UE_LOGI("[keypad_probe] VERDICT(round2): accept verb = %s | direct-write isAcc stuck=%d. "
            "If a verb flipped isAcc 0->1 (+door 0->1) -> receiver replays digits IN FOCUS then calls THAT "
            "verb (native LED+door). Else -> receiver direct-writes isAcc/isDeny (LED repaint = hands-on check; "
            "door handled by the door channel).",
            verb, accWrite ? 1 : 0);
}

}  // namespace coop::dev::keypad_probe
