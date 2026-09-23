// coop/dev/look_probe.cpp -- see coop/dev/look_probe.h.
//
// A target counts as held after it has been under the crosshair for a while, so sweeping the
// view across a room is not read as blinks; one that comes back within the window after letting
// go is a blink, and a longer absence is the player looking away. The HUD half watches the
// widget's own rebuild bodies at the script-body gate and counts each by who called it.

#include "coop/dev/look_probe.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/active_drive.h"     // NowMs
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::look_probe {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;

constexpr uint64_t kHeldMs   = 300;    // under the crosshair this long before a loss counts
constexpr uint64_t kBackMs   = 500;    // back within this and it was a blink
constexpr uint64_t kTallyMs  = 30000;
constexpr int      kBlinkLines = 80;

void*    g_cur = nullptr;
uint64_t g_curSince = 0;
void*    g_lost = nullptr;      // the held target that let go, while it may still come back
uint64_t g_lostAt = 0;
void*    g_standIn = nullptr;   // what the crosshair read instead
uint32_t g_ticksGone = 0;

uint64_t g_blinks = 0, g_losses = 0, g_nextTallyMs = 0;
int      g_lines = 0;
bool     g_isHost = false;

// The HUD bodies that tear down or rebuild what the hint shows: the action list, and the hover
// text over the aimed actor. Each call is keyed by the body and its caller.
const wchar_t* const kHudFns[] = {L"buildActions", L"clearActions", L"openHovertext", L"updateSlotInv"};
constexpr int kHudCount = static_cast<int>(sizeof(kHudFns) / sizeof(kHudFns[0]));
constexpr uint64_t kHudTallyMs = 10000;
constexpr int      kHudLines = 40;
bool     g_hudWatched = false;
uint64_t g_nextHudResolveMs = 0, g_nextHudTallyMs = 0;
int      g_hudLines = 0;
std::map<std::wstring, uint64_t> g_hudCalls;   // "body <- caller" -> calls this window

// The fields the player's LookAtFunction compares every frame before it rebuilds the HUD: all of
// them unchanged and only the side text is redrawn, any one changed and the action list is torn
// down and built again. lookAtState is one bit, "no active interface".
struct Field { const wchar_t* name; bool isObj; int32_t off = -1; uint64_t prev = 0; bool seen = false; uint64_t changes = 0; };
Field g_fields[] = {
    {L"activeInterface", true}, {L"lookAtState", false}, {L"lookAtVerify", false},
    {L"lookAtComponent", true}, {L"lookAtBoundsReplace", true},
};
bool g_fieldsResolved = false;
int  g_fieldLines = 0;
constexpr int kFieldLines = 60;

std::wstring Describe(void* actor) {
    if (!actor) return L"nothing";
    if (!R::IsLive(actor)) return L"(dead)";
    return R::ToString(R::NameOf(actor));
}

// A guest's world holds two mainPlayer_C: its own and the host's puppet, a full mainPlayer_C with
// its own tick. The HUD is one, so which of them called matters.
std::wstring WhoIs(void* obj) {
    auto& reg = coop::players::Registry::Get();
    if (obj == reg.Local()) return L" (the local player)";
    if (reg.IsPuppet(obj)) return L" (a PUPPET)";
    return L"";
}

std::wstring CallerOf(const SG::Call& c) {
    if (c.callerFunction) {
        std::wstring s = R::ToString(R::NameOf(c.callerFunction));
        if (c.callerObject) s += L" on " + R::ClassNameOf(c.callerObject) + WhoIs(c.callerObject);
        return s;
    }
    return c.fromOurCode ? L"our code" : L"an engine event";
}

std::wstring Show(const Field& f, uint64_t v) {
    if (!f.isObj) return std::to_wstring(v);
    return Describe(reinterpret_cast<void*>(v));
}

void TrackFields(void* player, uint64_t t) {
    if (!g_fieldsResolved) {
        g_fieldsResolved = true;
        void* cls = R::ClassOf(player);
        for (Field& f : g_fields) f.off = cls ? R::FindPropertyOffset(cls, f.name) : -1;
        UE_LOGI("[LOOK] FIELD offsets: activeInterface=%d lookAtState=%d lookAtVerify=%d "
                "lookAtComponent=%d lookAtBoundsReplace=%d", g_fields[0].off, g_fields[1].off,
                g_fields[2].off, g_fields[3].off, g_fields[4].off);
    }
    const uint8_t* base = static_cast<const uint8_t*>(player);
    for (Field& f : g_fields) {
        if (f.off < 0) continue;
        const uint64_t v = f.isObj ? reinterpret_cast<uint64_t>(*reinterpret_cast<void* const*>(base + f.off))
                                   : static_cast<uint64_t>(base[f.off]);
        if (f.seen && v != f.prev) {
            ++f.changes;
            if (++g_fieldLines <= kFieldLines)
                UE_LOGI("[LOOK] FIELD %s t=%llu %ls: %ls -> %ls", g_isHost ? "HOST" : "CLIENT",
                        static_cast<unsigned long long>(t), f.name, Show(f, f.prev).c_str(),
                        Show(f, v).c_str());
        }
        f.prev = v;
        f.seen = true;
    }
}

SG::Verdict OnHudPre(const SG::Call& c) {
    if (c.tag < 0 || c.tag >= kHudCount) return SG::Verdict::Run;
    const std::wstring key = std::wstring(kHudFns[c.tag]) + L" <- " + CallerOf(c);
    ++g_hudCalls[key];
    if (++g_hudLines <= kHudLines)
        UE_LOGI("[LOOK] HUD %s t=%llu %ls", g_isHost ? "HOST" : "CLIENT",
                static_cast<unsigned long long>(coop::active_drive::NowMs()), key.c_str());
    return SG::Verdict::Run;
}

// LookAtFunction itself, on the local player: the fields before each call set against the fields
// after the previous one say whether something else wrote them between the calls, and the
// fields after it set against the fields before it say what the call wrote itself.
constexpr int kTagLookAt = 50;
constexpr int kFieldCount = static_cast<int>(sizeof(g_fields) / sizeof(g_fields[0]));
uint64_t g_postVals[kFieldCount] = {};
uint64_t g_preVals[kFieldCount] = {};
bool     g_havePost = false;
uint64_t g_between[kFieldCount] = {}, g_byCall[kFieldCount] = {};
int      g_betweenLines = 0, g_byCallLines = 0;

void ReadFields(const void* player, uint64_t* out) {
    const uint8_t* base = static_cast<const uint8_t*>(player);
    for (int i = 0; i < kFieldCount; ++i) {
        const Field& f = g_fields[i];
        out[i] = f.off < 0 ? 0
               : f.isObj ? reinterpret_cast<uint64_t>(*reinterpret_cast<void* const*>(base + f.off))
                         : static_cast<uint64_t>(base[f.off]);
    }
}

SG::Verdict OnLookAtPre(const SG::Call& c) {
    if (!g_fieldsResolved || c.object != coop::players::Registry::Get().Local()) return SG::Verdict::Run;
    ReadFields(c.object, g_preVals);
    if (g_havePost)
        for (int i = 0; i < kFieldCount; ++i)
            if (g_preVals[i] != g_postVals[i]) {
                ++g_between[i];
                if (++g_betweenLines <= 40)
                    UE_LOGI("[LOOK] BETWEEN %s t=%llu %ls written outside LookAtFunction: %ls -> %ls",
                            g_isHost ? "HOST" : "CLIENT",
                            static_cast<unsigned long long>(coop::active_drive::NowMs()),
                            g_fields[i].name, Show(g_fields[i], g_postVals[i]).c_str(),
                            Show(g_fields[i], g_preVals[i]).c_str());
            }
    return SG::Verdict::Run;
}

void OnLookAtPost(const SG::Call& c) {
    if (!g_fieldsResolved || c.object != coop::players::Registry::Get().Local()) return;
    ReadFields(c.object, g_postVals);
    g_havePost = true;
    for (int i = 0; i < kFieldCount; ++i)
        if (g_postVals[i] != g_preVals[i]) {
            ++g_byCall[i];
            if (++g_byCallLines <= 40)
                UE_LOGI("[LOOK] CALL %s t=%llu LookAtFunction wrote %ls: %ls -> %ls",
                        g_isHost ? "HOST" : "CLIENT",
                        static_cast<unsigned long long>(coop::active_drive::NowMs()),
                        g_fields[i].name, Show(g_fields[i], g_preVals[i]).c_str(),
                        Show(g_fields[i], g_postVals[i]).c_str());
        }
}

void WatchHud(uint64_t now) {
    if (g_hudWatched || now < g_nextHudResolveMs) return;
    g_nextHudResolveMs = now + 3000;   // a class miss walks the object array
    if (!SG::IsInstalled()) return;
    void* cls = R::FindClass(L"ui_UI_C");
    if (!cls) return;
    int n = 0;
    for (int i = 0; i < kHudCount; ++i) {
        void* fn = R::FindDispatchFunction(cls, kHudFns[i], nullptr);
        if (fn && SG::Watch(fn, i, &OnHudPre, nullptr)) ++n;   // the tag is the body's index
    }
    bool lookAt = false;
    if (void* mp = R::FindClass(L"mainPlayer_C"))
        if (void* fn = R::FindDispatchFunction(mp, L"LookAtFunction", nullptr))
            lookAt = SG::Watch(fn, kTagLookAt, &OnLookAtPre, &OnLookAtPost);
    SG::SetEnabled(true);
    g_hudWatched = true;
    UE_LOGI("[LOOK] HUD watches: %d/%d bodies of ui_UI_C, LookAtFunction %s", n, kHudCount,
            lookAt ? "watched" : "NOT watched");
}

void HudTally(uint64_t now) {
    if (now < g_nextHudTallyMs) return;
    g_nextHudTallyMs = now + kHudTallyMs;
    if (g_hudCalls.empty()) return;
    std::vector<std::pair<std::wstring, uint64_t>> rows(g_hudCalls.begin(), g_hudCalls.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    UE_LOGI("[LOOK] HUD TALLY %s, last %llu s:", g_isHost ? "HOST" : "CLIENT",
            static_cast<unsigned long long>(kHudTallyMs / 1000));
    int printed = 0;
    for (const auto& r : rows) {
        if (++printed > 10) break;
        UE_LOGI("[LOOK]   %llu x %ls", static_cast<unsigned long long>(r.second), r.first.c_str());
    }
    g_hudCalls.clear();
    for (int i = 0; i < kFieldCount; ++i) {
        Field& f = g_fields[i];
        if (f.changes || g_between[i] || g_byCall[i])
            UE_LOGI("[LOOK]   field %ls: seen by the tick %llu, written between calls %llu, by the "
                    "call %llu", f.name, static_cast<unsigned long long>(f.changes),
                    static_cast<unsigned long long>(g_between[i]),
                    static_cast<unsigned long long>(g_byCall[i]));
        f.changes = 0;
        g_between[i] = g_byCall[i] = 0;
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::look_probe);
    return s;
}

void Tick(bool isHost) {
    if (!IsEnabled()) return;
    g_isHost = isHost;
    const uint64_t t = coop::active_drive::NowMs();
    WatchHud(t);
    HudTally(t);
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    TrackFields(player, t);
    void* now = E::ReadMainPlayerLookAtActor(player);
    if (g_nextTallyMs == 0) g_nextTallyMs = t + kTallyMs;

    if (g_lost) {
        if (now == g_lost) {
            ++g_blinks;
            if (++g_lines <= kBlinkLines)
                UE_LOGI("[LOOK] BLINK %s t=%llu target=%ls gone %llu ms over %u tick(s), read "
                        "instead: %ls", isHost ? "HOST" : "CLIENT",
                        static_cast<unsigned long long>(g_lostAt), Describe(g_lost).c_str(),
                        static_cast<unsigned long long>(t - g_lostAt), g_ticksGone,
                        Describe(g_standIn).c_str());
            g_lost = nullptr;
            g_cur = now;   // still held: the blink does not restart its clock
            return;
        }
        if (t - g_lostAt > kBackMs) g_lost = nullptr;   // the player looked away
        else ++g_ticksGone;
    }
    if (now != g_cur) {
        if (g_cur && !g_lost && t - g_curSince >= kHeldMs) {
            g_lost = g_cur;
            g_lostAt = t;
            g_standIn = now;
            g_ticksGone = 1;
            ++g_losses;
        }
        g_cur = now;
        g_curSince = t;
    }
    if (t >= g_nextTallyMs) {
        g_nextTallyMs = t + kTallyMs;
        if (g_losses)
            UE_LOGI("[LOOK] TALLY %s blinks=%llu losses=%llu (a loss that did not come back is the "
                    "player looking away)", isHost ? "HOST" : "CLIENT",
                    static_cast<unsigned long long>(g_blinks), static_cast<unsigned long long>(g_losses));
    }
}

}  // namespace coop::dev::look_probe
