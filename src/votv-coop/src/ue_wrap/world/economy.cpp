// ue_wrap/world/economy.cpp -- see ue_wrap/world/economy.h.

#include "ue_wrap/world/economy.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::economy {
namespace {

namespace R = ue_wrap::reflection;

// Cached gamemode pointer (singleton-per-session); revalidated via IsLive, re-walked
// via FindObjectByClass on a level transition. NOT a per-frame full-array scan (cached).
ue_wrap::CachedObjRef g_gm;
void* ResolveGamemode() {
    if (g_gm.Alive()) return g_gm.Raw();
    g_gm.Set(R::FindObjectByClass(L"mainGamemode_C"));
    return g_gm.Raw();
}

// Cached property offsets. Constant per BP class (mainGamemode_C / saveSlot_C never
// change at runtime; a level transition re-walks the gamemode pointer but resolves the
// SAME class), so resolve each ONCE -- FindPropertyOffset walks the class property list
// + super chain, which the project forbids on a per-frame path (TickHost runs every
// net-pump tick on the host; cf. ue_wrap/actors/vitals.cpp's same caching). -1 = unresolved.
int32_t g_offSave   = -1;
int32_t g_offPoints = -1;

// Resolve the live saveSlot + the Points field offset (offsets cached after first
// resolve). Returns the saveSlot ptr (or null) and fills *outOff with the Points offset.
void* ResolveSaveSlotAndPoints(int32_t* outOff) {
    void* gm = ResolveGamemode();
    if (!gm) return nullptr;
    if (g_offSave < 0) g_offSave = R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot");
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSave);
    if (!save || !R::IsLive(save)) return nullptr;
    if (g_offPoints < 0) g_offPoints = R::FindPropertyOffset(R::ClassOf(save), L"Points");
    if (g_offPoints < 0) return nullptr;
    if (outOff) *outOff = g_offPoints;
    return save;
}

}  // namespace

void* SaveSlotPtr() {  // the shared gamemode->saveSlot resolve, ptr only
    void* gm = ResolveGamemode();
    if (!gm) return nullptr;
    if (g_offSave < 0) g_offSave = R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot");
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSave);
    return (save && R::IsLive(save)) ? save : nullptr;
}

bool ReadPoints(int32_t* out) {
    if (!out) return false;
    int32_t off = -1;
    void* save = ResolveSaveSlotAndPoints(&off);
    if (!save) return false;
    *out = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(save) + off);
    return true;
}

bool WritePoints(int32_t value) {
    int32_t off = -1;
    void* save = ResolveSaveSlotAndPoints(&off);
    if (!save) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(save) + off) = value;
    return true;
}

bool AddPoints(int32_t amount) {
    void* gm = ResolveGamemode();
    if (!gm) return false;
    void* fn = R::FindFunction(R::ClassOf(gm), L"AddPoints");
    if (!fn) {
        UE_LOGW("economy: AddPoints UFunction not found on mainGamemode");
        return false;
    }
    // AmainGamemode_C::AddPoints(int32 Add) -- writes saveSlot.Points + BP side-effects.
    ue_wrap::ParamFrame f(fn);
    f.Set<int32_t>(L"Add", amount);
    // PROPAGATE the dispatch result. Returning true unconditionally would make every caller's
    // failure branch unreachable -- order_sync's "committed but not charged" guard among them,
    // which is the one instrument watching for that regression.
    return ue_wrap::Call(gm, f);
}

bool RefreshPointsHud() {
    // The HUD credit number (mainGamemode.playerInterface.text_points, a UTextBlock) is
    // push-updated ONLY by the BP credit-writer addPoints via SetText -- nothing re-evaluates it
    // per frame. The client balance mirror writes saveSlot.Points directly (WritePoints,
    // deliberately side-effect-free to avoid firing credit-earned UI/email), which leaves the
    // DISPLAYED number frozen at the old value. Re-run the BP's OWN repaint by adding ZERO:
    // lib_C::addPoints is exactly { saveSlot.points += Add; text_points.SetText(IntToText(points));
    // if (Add>=0) stats.total_points += Add else stats.points_spent += |Add| }. With Add=0 the
    // value is unchanged, the stat write is a no-op (+= 0, on the >=0 branch, so points_spent is
    // never touched), and the ONLY observable effect is the SetText repaint -- matching the native
    // formatting EXACTLY. The credit-earned side-effects WritePoints avoids are gated on a real
    // (non-zero) credit, so a zero add is clean. Preferred over hand-rolling Conv_IntToText ->
    // UTextBlock::SetText, which would risk a grouping/format mismatch + FText marshaling.
    return AddPoints(0);
}

}  // namespace ue_wrap::economy
