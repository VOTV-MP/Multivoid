// coop/dev/death_write_diff.cpp -- see coop/dev/death_write_diff.h. The storage shape, and
// why it is not the obvious one. A cell of four strings and a byte vector per object and
// field, over a million cells, retained hundreds of megabytes for the rest of the process,
// millions of heap blocks and a half-second single-frame stall; worse, it corrupted the drill
// it serves, since the death test grades a memory balloon by differencing the alive and dead
// window slopes, and the allocator settling after the snapshot burst showed up as alive-window
// slope spent on the instrument rather than the death. So object, class and field names are
// interned (an object's name was being stored about twenty times over), the captured bytes
// live in one arena, and a cell is five 32-bit fields. Per-class field lists are resolved once
// per class instead of once per object per class hop, which also removes millions of engine
// name renders per snapshot, the pattern behind this project's worst memory-balloon incident.

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

// A field wider than this is captured truncated. Nothing in the known census is anywhere near
// it; the cap exists so one pathological inline array cannot make a snapshot huge.
constexpr int32_t kMaxCellBytes = 256;

// The Outer-expansion depth from each root. A leaf widget sits at the user widget, its tree,
// then the widget, so 2 reaches every authored widget and 3 leaves headroom for a nested tree.
constexpr int kOuterDepth = 3;

struct FieldRec {
    std::wstring name;
    int32_t      offset;
    int32_t      size;      // bytes captured (<= kMaxCellBytes)
    int32_t      declared;  // ElementSize * ArrayDim as declared
    uint8_t      boolMask;  // non-zero => compare ONLY this bit of the byte at `offset`
};

// One class resolved once: its name and its own instance fields, with the bool masks already
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

// 20 bytes; everything wide is an index.
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

// The per-snapshot class resolution, cleared at the top of every snapshot: the key is a raw
// class pointer, and a blueprint class dies on world unload and its address can be recycled,
// which would hand back a byte and mask belonging to a different class, the very
// mis-attribution the mask exists to prevent (the repo's own class cache revalidates for the
// same reason). Rebuilding per snapshot is free: the measured cold snapshot was cheaper than
// the warm one, so this was never the cost driver; the allocations were.
std::unordered_map<void*, uint32_t> g_classId;

// The noise floor. The key is the object's own name with any trailing numeric instance suffix
// stripped, plus the field. Keyed on the declaring class and field it was a latent defect:
// visibility is declared on the base widget, so one key covered every widget in the game, and
// one flickering clock colon in the control window would have suppressed the one cell this
// module exists to find; it survived only because no widget's visibility happened to move in
// that window. Stripping the suffix collapses pooled or runtime-created objects onto one key
// so they match across windows despite being a different instance each time. It does not by
// itself keep authored widgets distinct, since a designer-default name is itself
// type-plus-digits; see NoiseKey for the Outer qualifier that restores distinctness.
std::unordered_set<std::wstring> g_noise;

std::wstring StripInstanceSuffix(const std::wstring& in) {
    std::wstring n = in;
    size_t i = n.size();
    while (i > 0 && n[i - 1] >= L'0' && n[i - 1] <= L'9') --i;
    // The bound also refuses an all-digit name, where stripping would leave nothing.
    if (i > 0 && i < n.size() && n[i - 1] == L'_') n.resize(i - 1);
    return n;
}

// The key is outer, object and field, both names stripped of their instance suffix. The Outer
// qualifier is not decoration, and leaving it out was the second time this key was too
// coarse: the suffix strip was introduced to collapse pooled objects on the claim that
// authored widgets have stable, distinct names, which is false for the UI framework, since a
// designer-default name is itself type-plus-digits and the strip eats it (one such name
// appears in ten different widget blueprints in this game). Unqualified, one key would again
// span the whole game, and any flicker in a control stretch would suppress every
// death-authored write to every other widget of that name. The Outer restores the
// distinction for free, since it is already walked.
std::wstring NoiseKey(const Cell& c) {
    const ObjRec& o = g_snap.objects[c.objId];
    return StripInstanceSuffix(o.outerName) + L"/" + StripInstanceSuffix(o.name) + L"." +
           g_snap.classes[c.classId].fields[c.fieldIdx].name;
}

// One object-array walk that answers three questions at once: the Outer-to-children map (so
// the per-call linear child walk is never paid), the per-class live count (the
// appeared-object axis), and the set of live user-widget descendants (the widget scope, a
// class-chain census rather than a name list).
struct WorldIndex {
    std::unordered_map<void*, std::vector<void*>> childrenOf;
    std::unordered_map<void*, int32_t>            classCount;
    std::vector<void*>                            userWidgets;
};

// `countOnly` skips the two outputs the diff never reads; most of the walk's measured cost
// was hundreds of thousands of pushes into a children map nobody consulted.
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
        // A cheap chain test; no name rendering, no allocation.
        if (R::IsDescendantOfAny(cls, bases, 1)) out.userWidgets.push_back(o);
    }
    return true;
}

// Resolve a class once per snapshot: its name, its own fields, and each field's real bool
// byte and mask. Returns an index into the snapshot's classes.
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
        // Only a one-byte field can be a packed bool, and the bool lookup is a linear walk with a
        // name render per compare, so this guard removes most of those calls.
        if (f.size == 1) {
            int32_t byteOff = 0;
            uint8_t mask = 0;
            if (R::FindBoolProperty(cls, f.name.c_str(), byteOff, mask) && mask != 0 &&
                byteOff == f.offset)
                fr.boolMask = mask;
            // A bool whose byte offset is not the field offset (a native bitfield spanning more
            // than one byte) deliberately falls through to a byte-wise compare: it can still
            // mis-attribute, a known residual rather than a handled case.
        }
        rec.fields.push_back(std::move(fr));
    }
    return id;
}

// Capture every instance field of `obj`, climbing the whole class chain: the field enumeration
// returns own members only, so the climb is the caller's job, and visibility lives on the base
// widget, not on the image that latched.
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

// The scope roots, per the header: the three non-widget owners the known census touches, plus
// every live user-widget descendant.
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
    // Reserve at the measured size rather than growing from small: the growth alone moved over a
    // million cells through about eleven reallocations.
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
    // Refuse across a world change, rather than produce a reading that looks like a reading. The
    // sessionless death arm's acceptance term is that the travel does run, so there the
    // snapshot's objects belong to a world destroyed seconds ago; worse than useless, since a
    // dying world's actors are not kill-flagged until the GC purge (world_identity.h), so
    // liveness would read true for a while and the diff would attribute findings to a world that
    // no longer exists. A gate that decides whether to act on the world, so per that header's
    // exception it fails closed.
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

    // Liveness once per object, by slot index, not per cell and never the bare check: a pointer
    // cached across ticks must never be probed with the bare check, which dereferences a possibly
    // freed object, and a co-resident crash reporter sees the first-chance fault before our
    // handler absorbs it (cached_obj_ref.h). These pointers are held for tens of seconds. The
    // index check reads slots only, and its identity compare also rejects the case the bare check
    // structurally cannot see: an address recycled by a different, smaller object, where the
    // following byte compare would read at an offset valid for the old class and abort the whole
    // diff mid-loop.
    std::vector<uint8_t> live(g_snap.objects.size(), 0);
    int died = 0;
    for (size_t i = 0; i < g_snap.objects.size(); ++i) {
        const ObjRec& o = g_snap.objects[i];
        live[i] = R::IsLiveByIndex(o.obj, o.internalIdx) ? 1 : 0;
        if (!live[i]) ++died;
    }

    // How many changed lines to print. The point of the first reading is to see the raw delta, so
    // this is generous; it exists only so a pathological run cannot bury the log.
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
            // A packed flag: compare only its own bit, or every bool sharing the byte reports its
            // neighbour's change as its own.
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
                // Print from the first differing byte, not from byte 0: a wide struct whose change
                // is in the tail rendered as two identical-looking values on a line that exists
                // only because they differ.
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
