// coop/dev/door_probe.cpp -- see coop/dev/door_probe.h.
//
// SURVEY mode (2026-06-04 deep-dig): the first run proved doorOpen(true) sets
// isMoving=1 but isOpened NEVER flips and isMoving never clears -- the `move`
// timeline starts but never finishes. Two questions remain: (a) is the timeline
// actually ADVANCING (its alpha changing) or frozen, and (b) is this every door
// or just basedoor_entrance? This survey calls doorOpen(true) on the first N live
// keyed doors and logs, per door, isOpened/isMoving + the move-timeline alpha
// (move_a @0x0340) over ~90 ticks, then a one-line verdict. Dev-only (RULE 3).

#include "coop/dev/door_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/call.h"
#include "ue_wrap/door.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <array>
#include <cstdint>
#include <string>

namespace coop::dev::door_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace D = ue_wrap::door;

bool ProbeEnabled() {
    static const bool s_enabled = coop::config::IsIniKeyTrue("door_probe");
    return s_enabled;
}

// --- resolved field offsets ------------------------------------------------
bool    g_installed = false;
void*   g_doorCls   = nullptr;
int32_t g_isOpened = -1, g_isMoving = -1, g_active = -1, g_autoclose = -1, g_sensorOverlaps = -1;
constexpr int32_t kMoveAlphaOff = 0x0340;  // float move_a_<guid> -- the move-timeline output value
void*   g_moveFinishFn = nullptr;          // door_C::move__FinishedFunc (force-complete the open?)
void*   g_setTickFn    = nullptr;          // AActor::SetActorTickEnabled(bool)

// --- survey state ----------------------------------------------------------
struct Slot { void* actor; int32_t idx; std::wstring key; };
std::array<Slot, 8> g_doors{};
int      g_doorCount = 0;
uint64_t g_tick = 0;
bool     g_collected = false;
int      g_cur = 0;        // current door index
int      g_curTick = -10;  // <0 = pre-open settle; 0..kWatch = watching; >=kWatch = next
float    g_alphaStart = 0.f;
bool     g_everOpened = false;
bool     g_alphaMoved = false;
bool     g_wasOpen    = false;

template <typename T> T ReadAt(void* o, int32_t off, T dflt = T{}) {
    if (!o || off < 0) return dflt;
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(o) + off);
}
int32_t ReadArrayNum(void* o, int32_t off) {
    if (!o || off < 0) return -1;
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(o) + off + 8);
}
int32_t Off(const wchar_t* name, int32_t fb) { int32_t o = R::FindPropertyOffset(g_doorCls, name); return o >= 0 ? o : fb; }

constexpr int kStartDelay = 240;
constexpr int kWatch      = 180;  // ticks watched per door after the hold (catch a delayed re-close)
constexpr int kSettle     = 25;   // pre-open settle ticks per door

}  // namespace

void Install() {
    if (!ProbeEnabled() || g_installed) return;
    g_doorCls = R::FindClass(L"door_C");
    if (!g_doorCls || !D::EnsureResolved()) return;
    g_isOpened       = Off(L"isOpened", 0x0350);
    g_isMoving       = Off(L"isMoving", 0x0351);
    g_active         = Off(L"Active", 0x0352);
    g_autoclose      = Off(L"autoclose", 0x0353);
    g_sensorOverlaps = Off(L"sensorOverlaps", 0x0418);
    g_moveFinishFn   = R::FindFunction(g_doorCls, L"move__FinishedFunc");
    g_setTickFn      = R::FindFunction(g_doorCls, L"SetActorTickEnabled");
    g_installed = true;
    UE_LOGI("[door_probe] SURVEY installed. isOpened@0x%X isMoving@0x%X autoclose@0x%X moveAlpha@0x%X moveFinish=%p setTick=%p",
            g_isOpened, g_isMoving, g_autoclose, kMoveAlphaOff, g_moveFinishFn, g_setTickFn);
}

void Tick() {
    if (!ProbeEnabled() || !g_installed) return;
    ++g_tick;
    if (g_tick < kStartDelay) return;

    // Collect the first N live keyed doors once.
    if (!g_collected) {
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n && g_doorCount < (int)g_doors.size(); ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj || !D::IsDoor(obj) || !R::IsLive(obj)) continue;
            const std::wstring nm = R::ToString(R::NameOf(obj));
            if (nm.rfind(L"Default__", 0) == 0) continue;
            std::wstring key = D::GetKeyString(obj);
            if (key.empty() || key == L"None") continue;
            g_doors[g_doorCount++] = Slot{ obj, R::InternalIndexOf(obj), key };
        }
        if (g_doorCount == 0) { if (g_tick % 60 == 0) UE_LOGW("[door_probe] no doors yet"); return; }
        g_collected = true;
        g_cur = 0; g_curTick = -kSettle;
        UE_LOGW("[door_probe] === SURVEY START: %d doors ===", g_doorCount);
        for (int i = 0; i < g_doorCount; ++i) UE_LOGI("[door_probe]   [%d] key='%ls'", i, g_doors[i].key.c_str());
    }

    if (g_cur >= g_doorCount) return;  // done
    Slot& d = g_doors[g_cur];
    if (!R::IsLiveByIndex(d.actor, d.idx)) { ++g_cur; g_curTick = -kSettle; return; }

    if (g_curTick < 0) {                          // settle, then fire the PRODUCTION HOLD sequence
        if (g_curTick == -1) {
            // Read the door_L mesh RelativeLocation (component @door+0x02A0, RelativeLocation
            // @0x011C) BEFORE vs AFTER ForceOpen -- a change proves the mesh physically moved
            // (the "nothing changed" visual fix: ForceOpen must call move__UpdateFunc to lerp it).
            auto meshLoc = [&](const char* tag) {
                auto dump = [&](const char* nm, int32_t off) {
                    void* comp = ReadAt<void*>(d.actor, off, nullptr);
                    if (!comp) { UE_LOGW("[door_probe]   %s null", nm); return; }
                    float lx = ReadAt<float>(comp, 0x011C), ly = ReadAt<float>(comp, 0x0120), lz = ReadAt<float>(comp, 0x0124);
                    float rp = ReadAt<float>(comp, 0x0128), ry = ReadAt<float>(comp, 0x012C), rr = ReadAt<float>(comp, 0x0130);
                    UE_LOGW("[door_probe]   %s %s loc=(%.1f,%.1f,%.1f) rot=(P%.1f,Y%.1f,R%.1f)", nm, tag, lx, ly, lz, rp, ry, rr);
                };
                dump("door_L     ", 0x02A0);  // UStaticMeshComponent* door_L
                dump("door_L_root", 0x02B0);  // UBillboardComponent* door_L_root (pivot)
                dump("door_R_root", 0x02B8);  // UBillboardComponent* door_R_root (pivot)
            };
            const bool preOpen = ReadAt<bool>(d.actor, g_isOpened);
            UE_LOGW("[door_probe] --- door[%d] key='%ls' preOpen=%d -> ForceOpen (mesh check) ---", g_cur, d.key.c_str(), preOpen);
            meshLoc("BEFORE");
            D::ForceOpen(d.actor);
            meshLoc("AFTER ");
            D::SuppressHostHeldDoor(d.actor);
            g_everOpened = ReadAt<bool>(d.actor, g_isOpened);
            g_wasOpen = g_everOpened;
        }
        ++g_curTick; return;
    }

    // Watch whether the held-open door STAYS open, and if it re-closes, capture what state did it.
    const bool opened = ReadAt<bool>(d.actor, g_isOpened);
    const bool moving = ReadAt<bool>(d.actor, g_isMoving);
    const bool autoc  = ReadAt<bool>(d.actor, g_autoclose);
    const float alpha = ReadAt<float>(d.actor, kMoveAlphaOff);
    const int32_t sN  = ReadArrayNum(d.actor, g_sensorOverlaps);
    if (g_wasOpen && !opened)
        UE_LOGW("[door_probe] *** door[%d] key='%ls' RE-CLOSED at t=%d despite hold (autoclose=%d sensorN=%d moving=%d alpha=%.3f) ***",
                g_cur, d.key.c_str(), g_curTick, autoc, sN, moving, alpha);
    if (g_curTick % 15 == 0)
        UE_LOGI("[door_probe] door[%d] t=%-3d isOpened=%d isMoving=%d autoclose=%d alpha=%.3f sensorN=%d",
                g_cur, g_curTick, opened, moving, autoc, alpha, sN);
    g_wasOpen = opened;

    if (++g_curTick >= kWatch) {
        UE_LOGW("[door_probe] === VERDICT door[%d] key='%ls': openedAfterForce=%d stillOpenAtEnd=%d ===",
                g_cur, d.key.c_str(), g_everOpened, opened);
        D::ReleaseHostHeldDoor(d.actor);  // restore + close, reset for next door
        ++g_cur; g_curTick = -kSettle;
        if (g_cur >= g_doorCount) UE_LOGW("[door_probe] === HOLD-TEST COMPLETE ===");
    }
}

}  // namespace coop::dev::door_probe
