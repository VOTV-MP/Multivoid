// coop/dev/radio_tower_probe.cpp -- see coop/dev/radio_tower_probe.h.

#include "coop/dev/radio_tower_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"  // TryGetActorLocation
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::radio_tower_probe {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
using ue_wrap::field_io::TArrayView;

// The tower's bodies, each declared by radiotower_C. Pre-only watches that count and say; none cancels.
UE_FACT_FN(kRowScramble,      "dev.radio_tower_probe", L"radiotower_C", L"scramble", Declared);
UE_FACT_FN(kRowBreakdown,     "dev.radio_tower_probe", L"radiotower_C", L"breakdown", Declared);
UE_FACT_FN(kRowSetBroken,     "dev.radio_tower_probe", L"radiotower_C", L"setBroken", Declared);
UE_FACT_FN(kRowGenPuzzle,     "dev.radio_tower_probe", L"radiotower_C", L"genPuzzle", Declared);
UE_FACT_FN(kRowGenFuses,      "dev.radio_tower_probe", L"radiotower_C", L"genFuses", Declared);
UE_FACT_FN(kRowTryToFix,      "dev.radio_tower_probe", L"radiotower_C", L"tryToFix", Declared);
UE_FACT_FN(kRowCompleteGame,  "dev.radio_tower_probe", L"radiotower_C", L"completeMinigame", Declared);
UE_FACT_FN(kRowSwitchSwitch,  "dev.radio_tower_probe", L"radiotower_C", L"switchSwitch", Declared);
UE_FACT_FN(kRowLeverDone,     "dev.radio_tower_probe", L"radiotower_C", L"TL_lever__FinishedFunc", Declared);
UE_FACT_FN(kRowResetEvent,    "dev.radio_tower_probe", L"radiotower_C", L"TL_reset__reset__EventFunc", Declared);
UE_FACT_FN(kRowPlayerUsedOn,  "dev.radio_tower_probe", L"radiotower_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowActionOption,  "dev.radio_tower_probe", L"radiotower_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowPlayerUse,     "dev.radio_tower_probe", L"radiotower_C", L"player_use", Declared);
UE_FACT_FN(kRowExplosion,     "dev.radio_tower_probe", L"radiotower_C", L"reachedByExplosion", Declared);

const ue_wrap::facts::Function* const kWatches[] = {
    &kRowScramble, &kRowBreakdown, &kRowSetBroken, &kRowGenPuzzle, &kRowGenFuses, &kRowTryToFix,
    &kRowCompleteGame, &kRowSwitchSwitch, &kRowLeverDone, &kRowResetEvent, &kRowPlayerUsedOn,
    &kRowActionOption, &kRowPlayerUse, &kRowExplosion,
};
constexpr int kCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
constexpr int kTagBase = 0x52540000;  // 'RT', plus the watch's index
constexpr int kMaxShown = 32;         // fuses and puzzle cells printed per state line

constexpr uint64_t kClientBreakAfterMs = 20000;  // a client's own break, when the host's never crossed
constexpr uint64_t kClientDoneAfterMs  = 15000;  // after the client's fix
constexpr uint64_t kHostDoneAfterMs    = 60000;  // after the host's break

enum class Mode : uint8_t { Off, Observe, FixClient };

Mode ModeOf() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::radio_tower_probe);
        if (v.empty()) return Mode::Off;
        return v == "fixclient" ? Mode::FixClient : Mode::Observe;
    }();
    return m;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Game thread only: the gate fires on it, and the pump ticks on it.
const char* g_role = "none";            // host, client, or none before a session runs
bool     g_registered = false, g_registerFailed = false, g_liveSettled = false;
uint64_t g_nextLiveMs = 0, g_nextReadMs = 0;
uint64_t g_calls[kCount] = {};

sg::Verdict OnPre(const sg::Call& c) {
    const int i = c.tag - kTagBase;
    if (i < 0 || i >= kCount) return sg::Verdict::Run;
    UE_LOGI("[RADIO-TOWER] %s CALL %ls #%llu", g_role, kWatches[i]->Name(),
            static_cast<unsigned long long>(++g_calls[i]));
    return sg::Verdict::Run;
}

// Registered once per process; a name watch holds its slots for good.
void EnsureWatches() {
    if (!g_registered && !g_registerFailed) {
        if (!sg::IsInstalled()) return;
        for (int i = 0; i < kCount; ++i) {
            if (!sg::WatchName(*kWatches[i], kTagBase + i, &OnPre, nullptr)) {
                g_registerFailed = true;
                UE_LOGE("[RADIO-TOWER] the gate refused the watch on %ls -- this run counts no calls",
                        kWatches[i]->Name());
                return;
            }
        }
        g_registered = true;
    }
    if (!g_registered || g_liveSettled) return;
    const uint64_t now = NowMs();
    if (now < g_nextLiveMs) return;
    g_nextLiveMs = now + 1000;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (int i = 0; i < kCount; ++i) {
        if (sg::NameWatchLive(*kWatches[i], kTagBase + i)) ++live;
        if (sg::NameWatchSettled(*kWatches[i], kTagBase + i)) ++settled;
    }
    if (settled < kCount) return;
    g_liveSettled = true;
    UE_LOGI("[RADIO-TOWER] %s %d of %d watches LIVE", g_role, live, kCount);
    for (int i = 0; i < kCount; ++i)
        if (!sg::NameWatchLive(*kWatches[i], kTagBase + i))
            UE_LOGW("[RADIO-TOWER] %s no watch on %ls (the class lacks it, or the table is full)", g_role,
                    kWatches[i]->Name());
}

struct Bool { int32_t off = -1; uint8_t mask = 0; };

// The offsets of the tower's class, resolved once per tower taken.
struct Members {
    Bool    broken, skip, animation;
    int32_t puzzleSize = -1, fuses = -1, puzzleData = -1;
};

ue_wrap::CachedObjRef g_tower;
Members  g_m;
bool     g_foundSaid = false;

uint8_t* Base(void* obj) { return reinterpret_cast<uint8_t*>(obj); }
bool ReadBit(void* obj, const Bool& b) { return (Base(obj)[b.off] & b.mask) != 0; }
const TArrayView& ArrayAt(void* obj, int32_t off) { return *reinterpret_cast<const TArrayView*>(Base(obj) + off); }

bool ResolveMembers(void* cls) {
    Members m;
    const wchar_t* missing = nullptr;
    auto bit = [&](Bool& b, const wchar_t* name) {
        if (!R::FindBoolProperty(cls, name, b.off, b.mask) || !b.mask) { b.off = -1; if (!missing) missing = name; }
    };
    auto off = [&](int32_t& o, const wchar_t* name) {
        o = R::FindPropertyOffset(cls, name);
        if (o < 0 && !missing) missing = name;
    };
    bit(m.broken, L"isBroken");
    bit(m.skip, L"skipPuzzle");
    bit(m.animation, L"animation");
    off(m.puzzleSize, L"puzzleSize");
    off(m.fuses, L"fuses");
    off(m.puzzleData, L"puzzleData");
    if (missing) {
        UE_LOGW("[RADIO-TOWER] %s radiotower_C is loaded but %ls did not resolve -- no readings this run", g_role,
                missing);
        return false;
    }
    g_m = m;
    return true;
}

// The gamemode's radiotower, taken once and kept while it lives. Null while the gamemode or the tower is not
// there; a new world's tower is a new object, so the members resolve again for it.
void* TakeTower() {
    if (void* held = g_tower.Get()) return held;
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return nullptr;
    void* gmCls = R::ClassOf(gm);
    const int32_t off = gmCls ? R::FindPropertyOffset(gmCls, L"radiotower") : -1;
    if (off < 0) return nullptr;
    void* rt = *reinterpret_cast<void* const*>(Base(gm) + off);
    if (!rt || !R::IsLive(rt)) return nullptr;
    g_foundSaid = false;
    if (!ResolveMembers(R::ClassOf(rt))) { g_tower.Reset(); return nullptr; }
    g_tower.Set(rt);
    return rt;
}

struct Reading {
    bool broken = false, skip = false, animation = false;
    int32_t sizeX = 0, sizeY = 0, fuseNum = 0, cellNum = 0;
    // The tower's script compares and writes fuses elements as bytes; a puzzle cell is a bool.
    char fuseText[kMaxShown * 4 + 8] = {};
    char litText[kMaxShown + 8] = {};
};

void Read(void* rt, Reading& r) {
    r.broken = ReadBit(rt, g_m.broken);
    r.skip = ReadBit(rt, g_m.skip);
    r.animation = ReadBit(rt, g_m.animation);
    r.sizeX = *reinterpret_cast<const int32_t*>(Base(rt) + g_m.puzzleSize);
    r.sizeY = *reinterpret_cast<const int32_t*>(Base(rt) + g_m.puzzleSize + 4);
    const TArrayView& f = ArrayAt(rt, g_m.fuses);
    const TArrayView& p = ArrayAt(rt, g_m.puzzleData);
    r.fuseNum = f.num;
    r.cellNum = p.num;
    size_t at = 0;
    for (int32_t i = 0; f.data && i < f.num && i < kMaxShown; ++i)
        at += static_cast<size_t>(std::snprintf(r.fuseText + at, sizeof(r.fuseText) - at, i ? ",%u" : "%u",
                                                static_cast<unsigned>(f.data[i])));
    for (int32_t i = 0; p.data && i < p.num && i < kMaxShown; ++i) r.litText[i] = p.data[i] ? '1' : '0';
}

bool Whole(const Reading& r) { return r.fuseNum > 0 && r.cellNum > 0 && r.sizeX > 0 && r.sizeY > 0; }

// The tower's own reflected verbs; a call is the game's body run as the game runs it.
bool CallVerb(void* rt, const wchar_t* name, bool breakIt) {
    void* fn = R::FindFunction(R::ClassOf(rt), name);
    if (!fn) { UE_LOGW("[RADIO-TOWER] %s radiotower_C has no %ls", g_role, name); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (breakIt) { f.Set<bool>(L"isBroken", true); f.Set<bool>(L"pullLever", true); }
    return ue_wrap::Call(rt, f);
}

bool     g_hostBroke = false, g_hostDone = false;
bool     g_clientBroke = false, g_clientFixed = false, g_clientDone = false;
uint64_t g_hostBreakMs = 0, g_clientReadyMs = 0, g_clientFixMs = 0;

void HostFixture(const coop::net::Session& session, void* rt, const Reading& r, uint64_t now) {
    if (!g_hostBroke) {
        if (!session.AnyWorldReadyPeer() || !Whole(r)) return;
        g_hostBroke = true; g_hostBreakMs = now;
        CallVerb(rt, L"setBroken", true);
        UE_LOGI("[RADIO-TOWER] host BROKE it");
    } else if (!g_hostDone && now - g_hostBreakMs >= kHostDoneAfterMs) {
        g_hostDone = true;
        UE_LOGI("[RADIO-TOWER] host DONE broken=%d", r.broken ? 1 : 0);
    }
}

void ClientFixture(void* rt, const Reading& r, uint64_t now) {
    if (!g_clientFixed) {
        if (!coop::net_pump::HasAnnouncedWorldReady() || !Whole(r)) return;
        if (!g_clientReadyMs) g_clientReadyMs = now;
        if (!r.broken) {
            // The b256 measurement: the host's break had not crossed within the wait, so the client breaks its own
            // copy and repairs it, which is what that run read on both peers.
            if (g_clientBroke || now - g_clientReadyMs < kClientBreakAfterMs) return;
            g_clientBroke = true;
            CallVerb(rt, L"setBroken", true);
            UE_LOGI("[RADIO-TOWER] client broke its own copy (the host's break had not crossed within the wait)");
            return;
        }
        g_clientFixed = true; g_clientFixMs = now;
        CallVerb(rt, L"completeMinigame", false);
        UE_LOGI("[RADIO-TOWER] client FIXED its copy");
    } else if (!g_clientDone && now - g_clientFixMs >= kClientDoneAfterMs) {
        g_clientDone = true;
        UE_LOGI("[RADIO-TOWER] client DONE broken=%d", r.broken ? 1 : 0);
    }
}

void Observe(const coop::net::Session& session, uint64_t now) {
    void* rt = TakeTower();
    if (!rt) return;
    Reading r;
    Read(rt, r);
    if (!g_foundSaid && Whole(r)) {  // before its begin-play the size is the default and the arrays are empty
        ue_wrap::FVector p{};
        ue_wrap::engine::TryGetActorLocation(rt, p);
        g_foundSaid = true;
        UE_LOGI("[RADIO-TOWER] %s found at (%.0f,%.0f,%.0f) puzzleSize=%dx%d fuses.Num=%d puzzleData.Num=%d",
                g_role, p.X, p.Y, p.Z, r.sizeX, r.sizeY, r.fuseNum, r.cellNum);
    }
    static char s_last[sizeof(Reading::fuseText) + sizeof(Reading::litText) + 48] = {};
    char now_[sizeof(s_last)];
    std::snprintf(now_, sizeof(now_), "broken=%d animation=%d skip=%d fuses=%s lit=%s", r.broken ? 1 : 0,
                  r.animation ? 1 : 0, r.skip ? 1 : 0, r.fuseText, r.litText);
    if (std::strcmp(now_, s_last) != 0) {
        std::memcpy(s_last, now_, std::strlen(now_) + 1);
        UE_LOGI("[RADIO-TOWER] %s state %s", g_role, now_);
    }
    if (ModeOf() != Mode::FixClient || !session.running()) return;
    if (session.role() == coop::net::Role::Host) HostFixture(session, rt, r, now);
    else ClientFixture(rt, r, now);
}

}  // namespace

void Tick(const coop::net::Session& session) {
    if (ModeOf() == Mode::Off) return;
    g_role = !session.running() ? "none" : session.role() == coop::net::Role::Host ? "host" : "client";
    static bool s_held = false;
    if (!s_held) { sg::Acquire("the radio tower probe"); s_held = true; }
    EnsureWatches();
    const uint64_t now = NowMs();
    if (now < g_nextReadMs) return;
    g_nextReadMs = now + 1000;
    Observe(session, now);
}

}  // namespace coop::dev::radio_tower_probe
