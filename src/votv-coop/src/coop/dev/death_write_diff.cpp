// coop/dev/death_write_diff.cpp -- see coop/dev/death_write_diff.h.
//
// STORAGE SHAPE, and why it is not the obvious one (post-ship audit, 2026-08-31).
//
// The first version stored a `Cell` of four std::wstring plus a std::vector<uint8_t>, one per
// (object, field). MEASURED on the real drill log: 285 bytes/cell x 1.22M cells = **+344 MB
// retained for the rest of the process**, ~3.6M heap blocks, and a 468/625 ms single-frame
// stall (the host's own `[HITCH]` line; fps 73 -> 35 in that second). That is bad anywhere,
// but here it was actively CORRUPTING THE DRILL IT SERVES: the death test grades a memory
// balloon (D6) by differencing the alive and dead window slopes, and the allocator settling
// after the snapshot burst showed up as -3.21 MB/s of alive slope -- 16% of D6's own 20 MB/s
// headroom, spent on the instrument rather than the death.
//
// So: object names, class names and field names are INTERNED (60k objects and ~5k classes
// against 1.22M cells -- an object's name was being stored ~20 times over), the captured bytes
// live in ONE arena, and a cell is five 32-bit fields. Per-class field lists are resolved ONCE
// per UClass instead of once per object per class hop, which also removes ~1.7M engine
// `FName::ToString` renders per snapshot -- the exact pattern behind this project's 19 GB
// RAM-balloon incident (`reflection.h`'s own "wstring bomb" note).

#include "coop/dev/death_write_diff.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"

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
namespace WID = ue_wrap::world_identity;

// A field wider than this is captured TRUNCATED. Nothing in the known census is anywhere
// near it; the cap exists so one pathological inline array cannot make a snapshot huge.
constexpr int32_t kMaxCellBytes = 256;

// Outer-expansion depth from each root. A leaf UWidget sits at UserWidget -> WidgetTree ->
// widget, so 2 reaches every authored widget and 3 leaves headroom for a nested tree.
constexpr int kOuterDepth = 3;

struct FieldRec {
    std::wstring name;
    int32_t      offset;
    int32_t      size;      // bytes captured (<= kMaxCellBytes)
    int32_t      declared;  // ElementSize * ArrayDim as declared
    uint8_t      boolMask;  // non-zero => compare ONLY this bit of the byte at `offset`
};

// One UClass resolved once: its name and its own instance fields with bool masks already
// attributed. Built on first encounter per snapshot and reused for every object of that class.
struct ClassRec {
    std::wstring          name;
    std::vector<FieldRec> fields;
};

struct ObjRec {
    void*        obj;
    int32_t      internalIdx;  // for IsLiveByIndex -- see the diff's liveness pass
    std::wstring name;
    std::wstring outerName;    // qualifies the noise key -- see NoiseKey()
};

// 20 bytes. Everything wide is an index.
struct Cell {
    uint32_t objId;
    uint32_t classId;
    uint32_t fieldIdx;
    uint32_t arenaOff;
    int32_t  size;
};

struct Snap {
    bool                               valid = false;
    uint32_t                           worldGen = 0;   // see the diff's world gate
    std::vector<ObjRec>                objects;
    std::vector<ClassRec>              classes;
    std::vector<Cell>                  cells;
    std::vector<uint8_t>               arena;
    int                                truncatedCells = 0;  // captured short -- see the log
    std::unordered_map<void*, int32_t> classCount;  // UClass* -> live instance count
};

Snap g_snap;

// Per-snapshot class resolution. CLEARED at the top of every Snapshot(): the key is a raw
// UClass pointer, and this repo's own class cache revalidates for exactly that reason
// (`reflection.cpp`: a cached class must still be live AND still carry the expected name
// before a walk trusts it) -- a BP UClass dies on world unload and its address can be
// recycled, which would hand us a {byte, mask} belonging to a different class. That is the
// very mis-attribution the mask exists to prevent. Rebuilding per snapshot is free: the
// MEASURED cold snapshot (empty cache) was CHEAPER than the warm one, so this was never the
// cost driver -- the allocations were.
std::unordered_map<void*, uint32_t> g_classId;

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
// Stripping the numeric suffix collapses pooled or runtime-created objects
// (`CanvasPanelSlot_2147457267`, `DebugMod_..._LogElement_C_2147457198`) onto one key so they
// match across windows despite being a different instance each time. It does NOT by itself
// keep authored widgets distinct -- that claim was made here and is FALSE, because a UMG
// designer-default name is itself `<Type>_<digits>`. See NoiseKey() for the Outer qualifier
// that actually restores distinctness.
std::unordered_set<std::wstring> g_noise;

std::wstring StripInstanceSuffix(const std::wstring& in) {
    std::wstring n = in;
    size_t i = n.size();
    while (i > 0 && n[i - 1] >= L'0' && n[i - 1] <= L'9') --i;
    // `i > 0` also refuses an all-digit name, where stripping would leave nothing.
    if (i > 0 && i < n.size() && n[i - 1] == L'_') n.resize(i - 1);
    return n;
}

// The key is OUTER/OBJECT.field, both names stripped of their instance suffix.
//
// The OUTER qualifier is not decoration, and leaving it out was the SECOND time this key was
// too coarse. Stripping `_<digits>` was introduced to collapse pooled objects, on the claim
// that "authored widgets have stable names and stay DISTINCT". That claim is false for UMG:
// a designer-default widget name IS `<Type>_<digits>`, so the strip eats it -- `Image_6` alone
// appears in ten different widget blueprints in this game (ui_console, ui_radar, ui_stats,
// ui_menu, ui_playerInventory, ...). Unqualified, `Image.Visibility` would again be ONE key
// across the whole game, and any `Image_N` flickering in a control stretch would suppress
// every death-authored write to every other `Image_N`. Qualifying by Outer restores
// `ui_radar/Image` vs `ui_stats/Image` for free -- the Outer is already walked.
std::wstring NoiseKey(const Cell& c) {
    const ObjRec& o = g_snap.objects[c.objId];
    return StripInstanceSuffix(o.outerName) + L"/" + StripInstanceSuffix(o.name) + L"." +
           g_snap.classes[c.classId].fields[c.fieldIdx].name;
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

// `countOnly` skips the two outputs the diff never reads -- most of its measured ~60 ms was
// 237k push_backs into a children map nobody consulted.
bool BuildIndex(WorldIndex& out, bool countOnly = false) {
    void* userWidgetCls = countOnly ? nullptr : R::FindClass(P::name::UserWidgetClass);
    if (!countOnly && !userWidgetCls) {
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
        if (countOnly) continue;
        void* outer = R::OuterOf(o);
        if (outer) out.childrenOf[outer].push_back(o);
        // Cheap chain test; no name rendering, no allocation (the wstring-bomb lesson).
        if (R::IsDescendantOfAny(cls, bases, 1)) out.userWidgets.push_back(o);
    }
    return true;
}

// Resolve a UClass ONCE per snapshot: its name, its own fields, and each field's real bool
// {byte, mask}. Returns an index into g_snap.classes.
uint32_t ClassIdFor(void* cls) {
    auto it = g_classId.find(cls);
    if (it != g_classId.end()) return it->second;

    const uint32_t id = static_cast<uint32_t>(g_snap.classes.size());
    g_classId[cls] = id;
    g_snap.classes.emplace_back();
    ClassRec& rec = g_snap.classes.back();
    rec.name = R::ToString(R::NameOf(cls));

    for (const auto& f : R::EnumerateStructFields(cls)) {
        if (f.offset < 0 || f.size <= 0) continue;
        FieldRec fr;
        fr.name     = f.name;
        fr.offset   = f.offset;
        fr.declared = f.size;
        fr.size     = f.size > kMaxCellBytes ? kMaxCellBytes : f.size;
        fr.boolMask = 0;
        // Only a 1-byte field can BE a packed bool, and FindBoolProperty is a linear walk
        // with a name render per compare -- so this guard removes ~90% of those calls.
        if (f.size == 1) {
            int32_t byteOff = 0;
            uint8_t mask = 0;
            if (R::FindBoolProperty(cls, f.name.c_str(), byteOff, mask) && mask != 0 &&
                byteOff == f.offset)
                fr.boolMask = mask;
            // A bool whose FBoolProperty ByteOffset is NOT the field offset (a native
            // bitfield spanning more than one byte) deliberately falls through to a
            // byte-wise compare: it can still mis-attribute, and that is a KNOWN residual
            // rather than a handled case.
        }
        rec.fields.push_back(std::move(fr));
    }
    return id;
}

// Capture every instance field of `obj`, climbing the whole class chain (EnumerateStructFields
// returns OWN members only, so the climb is the caller's job -- and `Visibility` lives on
// UWidget, not on the UImage that latched).
void CaptureObject(void* obj) {
    if (!obj || !R::IsLive(obj)) return;
    void* cls = R::ClassOf(obj);
    if (!cls) return;

    const uint32_t objId = static_cast<uint32_t>(g_snap.objects.size());
    void* outer = R::OuterOf(obj);
    g_snap.objects.push_back(ObjRec{obj, R::InternalIndexOf(obj),
                                    R::ToString(R::NameOf(obj)),
                                    outer ? R::ToString(R::NameOf(outer)) : std::wstring()});

    const auto* base = reinterpret_cast<const uint8_t*>(obj);
    for (int hop = 0; cls && hop < 24; ++hop, cls = R::SuperStructOf(cls)) {
        const uint32_t classId = ClassIdFor(cls);
        const auto& fields = g_snap.classes[classId].fields;
        for (uint32_t fi = 0; fi < fields.size(); ++fi) {
            const FieldRec& f = fields[fi];
            Cell c;
            c.objId    = objId;
            c.classId  = classId;
            c.fieldIdx = fi;
            c.size     = f.size;
            if (f.declared > f.size) ++g_snap.truncatedCells;
            c.arenaOff = static_cast<uint32_t>(g_snap.arena.size());
            g_snap.arena.insert(g_snap.arena.end(), base + f.offset,
                                base + f.offset + f.size);
            g_snap.cells.push_back(c);
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
    g_classId.clear();
    // Reserve at the MEASURED size rather than growing from 16k: the growth alone moved up to
    // 1.2M cells through ~11 reallocations.
    g_snap.cells.reserve(1300000);
    g_snap.arena.reserve(24u << 20);
    g_snap.objects.reserve(scope.size());
    for (void* o : scope) CaptureObject(o);
    g_snap.classCount = std::move(idx.classCount);
    g_snap.worldGen   = WID::Generation();
    g_snap.valid      = true;

    UE_LOGI("death_diff: SNAPSHOT -- %zu objects in scope (%zu of them UUserWidget "
            "descendants), %zu field cells over %zu classes, %.1f MB captured, "
            "%zu live classes",
            g_snap.objects.size(), idx.userWidgets.size(), g_snap.cells.size(),
            g_snap.classes.size(),
            (g_snap.cells.size() * sizeof(Cell) + g_snap.arena.size()) / (1024.0 * 1024.0),
            g_snap.classCount.size());
    return static_cast<int>(g_snap.cells.size());
}

void ResetNoiseFloor() { g_noise.clear(); }

void Release() {
    g_snap = Snap{};
    g_classId.clear();
    g_classId.rehash(0);
}

int DiffAndLog(const char* label, bool learnNoise) {
    if (!g_snap.valid) {
        UE_LOGW("death_diff: DIFF(%s) -- no snapshot to compare against", label ? label : "?");
        return -1;
    }
    // REFUSE ACROSS A WORLD CHANGE, rather than produce a reading that looks like a reading.
    // `mp.py death` WITHOUT `--session` is a mandatory arm whose acceptance term is that the
    // travel DOES run (D3, single-player untouched) -- so there the snapshot's objects belong
    // to a world that was destroyed seconds ago. Worse than useless: a dying world's actors are
    // NOT kill-flagged until GC purge (world_identity.h), so liveness would read TRUE for a
    // while and the diff would attribute findings to a world that no longer exists.
    // This is a gate that decides WHETHER TO ACT ON THE WORLD, so per that header's stated
    // exception it fails CLOSED.
    if (WID::Generation() != g_snap.worldGen) {
        UE_LOGW("death_diff: DIFF(%s) REFUSED -- the world changed since the snapshot "
                "(generation %u -> %u). A travel ran, so every cached object belongs to a dead "
                "world; there is no honest diff to report. This is expected in the SESSIONLESS "
                "arm, where the travel is supposed to run.",
                label ? label : "?", g_snap.worldGen, WID::Generation());
        return -1;
    }

    WorldIndex idx;
    const bool haveIdx = !learnNoise && BuildIndex(idx, /*countOnly=*/true);

    // LIVENESS ONCE PER OBJECT, BY SLOT INDEX -- not per cell, and never bare `IsLive`.
    //
    // `ue_wrap/core/cached_obj_ref.h` is explicit: a pointer cached across ticks must never be
    // probed with bare IsLive, because IsLive DEREFERENCES a possibly-GC-freed object and a
    // co-resident VEH crash reporter sees the first-chance AV before our SEH absorbs it. These
    // pointers are held 10-22 seconds. `IsLiveByIndex` is slot-reads-only, and its `*item ==
    // obj` compare also rejects the case IsLive structurally cannot see: an address RECYCLED by
    // a different, smaller object, where the following memcmp would read at an offset valid for
    // the old class and abort the whole diff mid-loop through RunTaskSEH.
    std::vector<uint8_t> live(g_snap.objects.size(), 0);
    int died = 0;
    for (size_t i = 0; i < g_snap.objects.size(); ++i) {
        const ObjRec& o = g_snap.objects[i];
        live[i] = R::IsLiveByIndex(o.obj, o.internalIdx) ? 1 : 0;
        if (!live[i]) ++died;
    }

    // How many changed lines we are willing to print. The point of the first reading is to
    // see the RAW delta, so this is generous; it exists only so a pathological run cannot
    // bury the log.
    constexpr int kMaxLines = 400;

    int changed = 0, printed = 0, truncatedCells = 0;
    int suppressed = 0, attributable = 0;
    UE_LOGI("death_diff: ==== DIFF BEGIN (%s)%s ====", label ? label : "?",
            learnNoise ? " -- LEARNING THE NOISE FLOOR, findings suppressed by design" : "");

    for (const auto& c : g_snap.cells) {
        if (!live[c.objId]) continue;
        const auto* base = reinterpret_cast<const uint8_t*>(g_snap.objects[c.objId].obj);
        const FieldRec& f = g_snap.classes[c.classId].fields[c.fieldIdx];
        const uint8_t* was = g_snap.arena.data() + c.arenaOff;

        if (f.boolMask) {
            // A packed flag: compare ONLY its own bit, or every bool sharing the byte reports
            // its neighbour's change as its own.
            if (((base[f.offset] ^ was[0]) & f.boolMask) == 0) continue;
        } else if (std::memcmp(base + f.offset, was, static_cast<size_t>(c.size)) == 0) {
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
        if (f.declared > c.size) ++truncatedCells;
        if (printed < kMaxLines) {
            ++printed;
            const wchar_t* objName = g_snap.objects[c.objId].name.c_str();
            const wchar_t* clsName = g_snap.classes[c.classId].name.c_str();
            if (f.boolMask) {
                UE_LOGI("death_diff:   %ls.%ls  (%ls)  off=0x%X bit=0x%02X  %d -> %d",
                        objName, f.name.c_str(), clsName, f.offset, f.boolMask,
                        (was[0] & f.boolMask) ? 1 : 0, (base[f.offset] & f.boolMask) ? 1 : 0);
            } else {
                // Print from the FIRST DIFFERING byte, not from byte 0. A 136-byte struct
                // whose change is in the tail rendered as `906C... -> 906C...` -- two
                // identical-looking values on a line that exists only because they differ.
                int d0 = 0;
                while (d0 < c.size && base[f.offset + d0] == was[d0]) ++d0;
                wchar_t a[24] = {0}, b[24] = {0};
                const int show = (c.size - d0) < 8 ? (c.size - d0) : 8;
                for (int i = 0; i < show; ++i) {
                    swprintf(a + i * 2, 3, L"%02X", was[d0 + i]);
                    swprintf(b + i * 2, 3, L"%02X", base[f.offset + d0 + i]);
                }
                UE_LOGI("death_diff:   %ls.%ls  (%ls)  off=0x%X sz=%d  @+%d: %ls -> %ls",
                        objName, f.name.c_str(), clsName, f.offset, f.declared, d0, a, b);
            }
        }
    }

    int appearedClasses = 0;
    if (haveIdx) {
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
                "simply standing there, giving %zu (object.field) keys that CANNOT be "
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
    return attributable;
}

}  // namespace coop::dev::death_write_diff
