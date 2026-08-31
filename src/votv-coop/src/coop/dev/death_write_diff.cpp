// coop/dev/death_write_diff.cpp -- see coop/dev/death_write_diff.h.

#include "coop/dev/death_write_diff.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::dev::death_write_diff {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// A field wider than this is captured TRUNCATED. Nothing in the known census is anywhere
// near it; the cap exists so one pathological inline array cannot make a snapshot huge.
constexpr int32_t kMaxCellBytes = 256;

// Outer-expansion depth from each root. A leaf UWidget sits at UserWidget -> WidgetTree ->
// widget, so 2 reaches every authored widget and 3 leaves headroom for a nested tree.
constexpr int kOuterDepth = 3;

struct Cell {
    void*        obj;
    std::wstring objName;
    std::wstring clsName;
    std::wstring fieldName;
    int32_t      offset;
    int32_t      size;      // bytes actually captured (<= kMaxCellBytes)
    int32_t      declared;  // ElementSize * ArrayDim as declared
    uint8_t      boolMask;  // non-zero => compare ONLY this bit of the byte at `offset`
    std::vector<uint8_t> bytes;
};

// Per-class bool bitfield map, built once per UClass and cached.
//
// WHY THIS EXISTS. A `uint8 flag : 1` UPROPERTY reports offset+size of the whole BYTE, and
// several flags pack into one byte -- so a byte-wise cell reports ONE change once per flag
// sharing that byte, and attributes it to all of them. In the first reading that produced
// `bNetAddressable C7 -> 07` and `bReplicates C7 -> 07` (the same byte, twice) and, worse, a
// false lead: `ui_UI_C.Hidden 00 -> 01` looked like an unhandled HUD write until a bytecode
// search found that NOTHING in the game writes `ui_UI_C.Hidden` -- some other bool in the
// byte at 0x5B8 had moved. An instrument that cannot say WHICH flag changed manufactures
// findings (`[[lesson-a-signature-match-is-class-membership-not-attribution]]`).
//
// `FindBoolProperty` gives the real {byteOffset, mask}; it walks linearly, so the result is
// cached per class -- 1.2M per-field calls would otherwise cost a walk each.
std::unordered_map<void*, std::unordered_map<std::wstring, uint8_t>> g_boolMaskCache;

const std::unordered_map<std::wstring, uint8_t>& BoolMasksFor(void* cls) {
    auto it = g_boolMaskCache.find(cls);
    if (it != g_boolMaskCache.end()) return it->second;
    auto& m = g_boolMaskCache[cls];
    for (const auto& f : R::EnumerateStructFields(cls)) {
        int32_t byteOff = 0;
        uint8_t mask = 0;
        if (R::FindBoolProperty(cls, f.name.c_str(), byteOff, mask) && mask != 0 &&
            byteOff == f.offset)
            m[f.name] = mask;
    }
    return m;
}

struct Snap {
    bool                                  valid = false;
    std::vector<Cell>                     cells;
    std::unordered_set<void*>             objects;
    std::unordered_map<void*, int32_t>    classCount;  // UClass* -> live instance count
};

Snap g_snap;

// The noise floor. The key is the object's OWN NAME with any trailing `_<digits>` instance
// suffix stripped, plus the field.
//
// The first version keyed on DECLARING CLASS + field, and that was a latent instrument
// defect: `Visibility` is declared on `UWidget`, so the single key `Widget.Visibility` covers
// EVERY widget in the game. One flickering clock colon in the control window would have
// suppressed `dmg_full.Visibility` -- the one cell this whole module exists to find. It
// survived the first run only because no widget's visibility happened to move in that
// window, which is luck, not design (`[[lesson-an-instrument-that-shares-the-defect-cancels-
// it]]`, and `[[lesson-a-sentinel-guards-the-failure-you-imagined-not-the-one-you-get]]`).
//
// Stripping only the numeric suffix keeps both halves working: authored widgets
// (`dmg_full`, `vbox_actionList`, `text_clockTime_colon`) have stable names and stay
// DISTINCT, while pooled or runtime-created objects (`CanvasPanelSlot_2147457267`,
// `DebugMod_..._LogElement_C_2147457198`) collapse onto one key so they still match across
// windows despite being different instances each time.
std::unordered_set<std::wstring> g_noise;

std::wstring NoiseKey(const Cell& c) {
    std::wstring n = c.objName;
    size_t i = n.size();
    while (i > 0 && n[i - 1] >= L'0' && n[i - 1] <= L'9') --i;
    if (i > 0 && i < n.size() && n[i - 1] == L'_') n.resize(i - 1);
    return n + L"." + c.fieldName;
}

// ---------------------------------------------------------------------------------------
// One GUObjectArray walk that answers three questions at once: the Outer->children map (so
// we never pay ChildObjectsOf's per-call linear walk), the per-class live count (the
// "appeared object" axis), and the set of live UUserWidget descendants (the widget scope, as
// a class-chain census rather than a name list).
struct WorldIndex {
    std::unordered_map<void*, std::vector<void*>> childrenOf;
    std::unordered_map<void*, int32_t>            classCount;
    std::vector<void*>                            userWidgets;
};

bool BuildIndex(WorldIndex& out) {
    void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
    if (!userWidgetCls) {
        UE_LOGW("death_diff: UserWidget class unresolved -- scope cannot be built");
        return false;
    }
    void* const bases[1] = {userWidgetCls};

    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLiveByIndex(o, i)) continue;
        void* cls = R::ClassOf(o);
        if (!cls) continue;
        ++out.classCount[cls];
        void* outer = R::OuterOf(o);
        if (outer) out.childrenOf[outer].push_back(o);
        // Cheap chain test; no name rendering, no allocation (the wstring-bomb lesson).
        if (R::IsDescendantOfAny(cls, bases, 1)) out.userWidgets.push_back(o);
    }
    return true;
}

// Capture every instance field of `obj`, climbing the whole class chain (EnumerateStructFields
// returns OWN members only, so the climb is the caller's job -- and `Visibility` lives on
// UWidget, not on the UImage that latched).
void CaptureObject(void* obj, std::vector<Cell>& into) {
    if (!obj || !R::IsLive(obj)) return;
    void* cls = R::ClassOf(obj);
    if (!cls) return;

    const std::wstring objName = R::ToString(R::NameOf(obj));
    auto* base = reinterpret_cast<const uint8_t*>(obj);

    for (int hop = 0; cls && hop < 24; ++hop, cls = R::SuperStructOf(cls)) {
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        const auto& boolMasks = BoolMasksFor(cls);
        for (const auto& f : R::EnumerateStructFields(cls)) {
            if (f.offset < 0 || f.size <= 0) continue;
            const int32_t take = f.size > kMaxCellBytes ? kMaxCellBytes : f.size;
            Cell c;
            c.obj       = obj;
            c.objName   = objName;
            c.clsName   = clsName;
            c.fieldName = f.name;
            c.offset    = f.offset;
            c.size      = take;
            c.declared  = f.size;
            const auto bm = boolMasks.find(f.name);
            c.boolMask  = bm == boolMasks.end() ? 0 : bm->second;
            c.bytes.resize(static_cast<size_t>(take));
            std::memcpy(c.bytes.data(), base + f.offset, static_cast<size_t>(take));
            into.push_back(std::move(c));
        }
    }
}

// The scope roots, per the header: the three non-widget owners the known census touches,
// plus every live UUserWidget descendant.
void CollectRoots(const WorldIndex& idx, std::vector<void*>& roots) {
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) roots.push_back(gi);
    if (void* gm = R::FindObjectByClass(P::name::GamemodeClass)) roots.push_back(gm);
    if (void* mp = coop::players::Registry::Get().Local(); mp && R::IsLive(mp)) roots.push_back(mp);
    for (void* w : idx.userWidgets) roots.push_back(w);
}

void ExpandOverOuters(const WorldIndex& idx, const std::vector<void*>& roots,
                      std::unordered_set<void*>& out) {
    std::vector<void*> frontier = roots;
    for (void* r : roots) out.insert(r);
    for (int depth = 0; depth < kOuterDepth && !frontier.empty(); ++depth) {
        std::vector<void*> next;
        for (void* o : frontier) {
            auto it = idx.childrenOf.find(o);
            if (it == idx.childrenOf.end()) continue;
            for (void* c : it->second)
                if (out.insert(c).second) next.push_back(c);
        }
        frontier.swap(next);
    }
}

}  // namespace

int Snapshot() {
    WorldIndex idx;
    if (!BuildIndex(idx)) return -1;

    std::vector<void*> roots;
    CollectRoots(idx, roots);
    if (roots.empty()) {
        UE_LOGW("death_diff: no scope roots resolved -- snapshot skipped");
        return -1;
    }

    std::unordered_set<void*> scope;
    ExpandOverOuters(idx, roots, scope);

    g_snap = Snap{};
    g_snap.cells.reserve(16384);
    for (void* o : scope) CaptureObject(o, g_snap.cells);
    g_snap.objects    = std::move(scope);
    g_snap.classCount = std::move(idx.classCount);
    g_snap.valid      = true;

    UE_LOGI("death_diff: SNAPSHOT -- %zu objects in scope (%zu of them UUserWidget "
            "descendants), %zu field cells, %zu live classes",
            g_snap.objects.size(), idx.userWidgets.size(), g_snap.cells.size(),
            g_snap.classCount.size());
    return static_cast<int>(g_snap.cells.size());
}

void ResetNoiseFloor() { g_noise.clear(); }

int DiffAndLog(const char* label, bool learnNoise) {
    if (!g_snap.valid) {
        UE_LOGW("death_diff: DIFF(%s) -- no snapshot to compare against", label ? label : "?");
        return -1;
    }

    WorldIndex idx;
    const bool haveIdx = BuildIndex(idx);

    // How many changed lines we are willing to print. The point of the first reading is to
    // see the RAW delta, so this is generous; it exists only so a pathological run cannot
    // bury the log.
    constexpr int kMaxLines = 400;

    int changed = 0, died = 0, printed = 0, truncatedCells = 0;
    int suppressed = 0, attributable = 0;
    UE_LOGI("death_diff: ==== DIFF BEGIN (%s)%s ====", label ? label : "?",
            learnNoise ? " -- LEARNING THE NOISE FLOOR, findings suppressed by design" : "");

    for (const auto& c : g_snap.cells) {
        if (!R::IsLive(c.obj)) {
            ++died;
            continue;
        }
        const auto* base = reinterpret_cast<const uint8_t*>(c.obj);
        if (c.boolMask) {
            // A packed flag: compare ONLY its own bit, or every bool sharing the byte reports
            // its neighbour's change as its own.
            if (((base[c.offset] ^ c.bytes[0]) & c.boolMask) == 0) continue;
        } else if (std::memcmp(base + c.offset, c.bytes.data(),
                               static_cast<size_t>(c.size)) == 0) {
            continue;
        }
        ++changed;
        if (learnNoise) {
            g_noise.insert(NoiseKey(c));
            continue;  // learning the floor: count it, never print it as a finding
        }
        if (g_noise.count(NoiseKey(c))) {
            ++suppressed;
            continue;
        }
        ++attributable;
        if (c.declared > c.size) ++truncatedCells;
        if (printed < kMaxLines) {
            ++printed;
            // Print from the FIRST DIFFERING byte, not from byte 0. A 136-byte struct whose
            // change is in the tail rendered as `906C... -> 906C...` -- two identical-looking
            // values on a line that exists only because they differ, which is an instrument
            // that hides its own finding.
            int d0 = 0;
            while (d0 < c.size && base[c.offset + d0] == c.bytes[static_cast<size_t>(d0)]) ++d0;
            wchar_t was[24] = {0}, now[24] = {0};
            const int show = (c.size - d0) < 8 ? (c.size - d0) : 8;
            for (int i = 0; i < show; ++i) {
                swprintf(was + i * 2, 3, L"%02X", c.bytes[static_cast<size_t>(d0 + i)]);
                swprintf(now + i * 2, 3, L"%02X", base[c.offset + d0 + i]);
            }
            if (c.boolMask)
                UE_LOGI("death_diff:   %ls.%ls  (%ls)  off=0x%X bit=0x%02X  %d -> %d",
                        c.objName.c_str(), c.fieldName.c_str(), c.clsName.c_str(), c.offset,
                        c.boolMask, (c.bytes[0] & c.boolMask) ? 1 : 0,
                        (base[c.offset] & c.boolMask) ? 1 : 0);
            else
                UE_LOGI("death_diff:   %ls.%ls  (%ls)  off=0x%X sz=%d  @+%d: %ls -> %ls",
                        c.objName.c_str(), c.fieldName.c_str(), c.clsName.c_str(), c.offset,
                        c.declared, d0, was, now);
        }
    }

    int appearedClasses = 0;
    if (haveIdx && !learnNoise) {
        for (const auto& kv : idx.classCount) {
            const auto prev = g_snap.classCount.find(kv.first);
            const int32_t before = prev == g_snap.classCount.end() ? 0 : prev->second;
            if (kv.second == before) continue;
            ++appearedClasses;
            if (printed < kMaxLines) {
                ++printed;
                UE_LOGI("death_diff:   [class count] %ls  %d -> %d",
                        R::ToString(R::NameOf(kv.first)).c_str(), before, kv.second);
            }
        }
    }

    if (learnNoise) {
        UE_LOGI("death_diff: ==== NOISE FLOOR LEARNED (%s) -- %d cells moved with the player "
                "simply standing there, giving %zu (class.field) keys that CANNOT be "
                "attributed to a death ====",
                label ? label : "?", changed, g_noise.size());
        return changed;
    }
    UE_LOGI("death_diff: ==== DIFF END (%s) -- %d cells CHANGED of %zu; %d SUPPRESSED as "
            "known world churn, leaving %d DEATH-ATTRIBUTABLE; %d snapshot objects died, "
            "%d class counts moved%s ====",
            label ? label : "?", changed, g_snap.cells.size(), suppressed, attributable, died,
            appearedClasses, printed >= kMaxLines ? " [OUTPUT CAPPED]" : "");
    if (truncatedCells > 0)
        UE_LOGI("death_diff: note -- %d changed cells were captured TRUNCATED at %d bytes; "
                "their tails were not compared", truncatedCells, kMaxCellBytes);
    return changed;
}

}  // namespace coop::dev::death_write_diff
