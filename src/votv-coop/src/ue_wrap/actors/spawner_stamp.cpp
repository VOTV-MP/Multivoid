// ue_wrap/actors/spawner_stamp.cpp -- see ue_wrap/actors/spawner_stamp.h.

#include "ue_wrap/actors/spawner_stamp.h"

#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cwchar>
#include <string>

namespace ue_wrap::spawner_stamp {
namespace {

namespace R  = reflection;
namespace OI = object_index;

// The two functions a row stands on, for the version census. The GUARD row is the class's own function the guarded
// path runs in (its BeginPlay stub, whose path compares the constant): a stamp is made only while the loaded class
// still declares it. The SPAWNER row is the spawner's whole graph (its stub is not singled out, so it moves on any
// edit of the graph): the census's, kept in the table so a change to what the spawner writes is seen.
UE_FACT_FN(kRowOrborbBeginPlay, "spawner_stamp", L"npc_orborb_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowBlackballSpawner, "spawner_stamp", L"blackballSpawner_C", L"ExecuteUbergraph_blackballSpawner",
           Declared);
UE_FACT_FN(kRowBirchBeginPlay, "spawner_stamp", L"birch_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowBirchSpawner, "spawner_stamp", L"birchSpawner_C", L"ExecuteUbergraph_birchSpawner", Declared);

// One class's native spawner: the FString property it writes, the constant, and the two fact rows the constant
// depends on. blackballSpawner_C's ubergraph writes NewVar_0 between the spawn's halves and npc_orborb_C's BeginPlay
// compares it, else ends the game; birchSpawner_C and birch_C are the same shape.
struct Row {
    const wchar_t*         cls;
    const wchar_t*         prop;
    const wchar_t*         value;
    const facts::Function& guard;
    const facts::Function& spawner;
};
const Row kRows[] = {
    {L"npc_orborb_C", L"NewVar_0", L"O0ttQI_-jnVwHSztT-bb_w", kRowOrborbBeginPlay, kRowBlackballSpawner},
    {L"birch_C", L"NewVar_0", L"1nQmIuuHPiU9vlTy4ubQ5g==", kRowBirchBeginPlay, kRowBirchSpawner},
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);

// Each row's class as an FName ({0,0} until converted): an index compare tells a spawn's class from the row's.
R::FName g_rowNames[kRowCount] = {};
bool     g_rowAsked[kRowCount] = {};

// What a row learned about one listing of its class.
struct Entry {
    void*    cls = nullptr;
    uint64_t listing = 0;
    int32_t  off = -1;  // -1: the guard function, the property or its FString type is missing
    const wchar_t* missing = L"";  // what is missing when off is -1: L"guard" or the property's name
    bool     saidStamped = false;
    bool     saidFailed = false;
};
Entry g_entries[kRowCount];
int   g_failures = 0;

// The row of `cls`, or -1. A row's name converts once, after the string library is up (the engine's name pool is
// process-wide, so it holds across worlds); it is asked once only, so a conversion that failed is not asked again
// on every spawn. A row with no converted name is compared by rendering the class's, so a class with a row is never
// passed over for want of its FName.
int RowOf(void* cls) {
    const R::FName& name = R::NameOf(cls);
    for (int i = 0; i < kRowCount; ++i) {
        R::FName& rowName = g_rowNames[i];
        if (!g_rowAsked[i] && fname_utils::Resolved()) {
            g_rowAsked[i] = true;
            rowName = fname_utils::StringToFName(kRows[i].cls);
        }
        const bool same = rowName.ComparisonIndex != 0
                              ? name.ComparisonIndex == rowName.ComparisonIndex && name.Number == rowName.Number
                              : R::NameEquals(name, kRows[i].cls);
        if (same) return i;
    }
    return -1;
}

// The property's offset when the class still declares the row's guard function and the property is an FString,
// else -1, with `what` naming the missing piece. Names compare without case, as the engine's do.
int32_t StringOffset(const Row& row, void* cls, const wchar_t*& what) {
    what = L"guard";
    if (!facts::ResolveOn(row.guard, cls)) return -1;
    what = row.prop;
    int32_t      off = -1;
    std::wstring type;
    if (!R::PropertyOffsetAndClass(cls, row.prop, off, type)) return -1;
    return ::_wcsicmp(type.c_str(), L"StrProperty") == 0 ? off : -1;
}

}  // namespace

bool Stamp(void* actorClass, void* deferredActor) {
    UE_ASSERT_GAME_THREAD("spawner_stamp::Stamp");
    const int index = actorClass ? RowOf(actorClass) : -1;
    if (index < 0) return true;
    const Row& row = kRows[index];
    Entry& e = g_entries[index];
    // A class the index does not list yet (id 0) cannot be told from a reloaded one: it is resolved afresh on every
    // call, its once-lines kept for as long as it is the same class object.
    const uint64_t listing = OI::ClassListingId(actorClass);
    const bool sameListing = e.cls == actorClass && e.listing == listing;
    if (listing == 0 || !sameListing) {
        const bool keepSaid = listing == 0 && sameListing;
        const bool saidStamped = keepSaid && e.saidStamped;
        const bool saidFailed = keepSaid && e.saidFailed;
        const wchar_t* missing = L"";
        const int32_t off = StringOffset(row, actorClass, missing);
        e = Entry{actorClass, listing, off, missing, saidStamped, saidFailed};
    }
    if (deferredActor && e.off >= 0 && field_io::WriteFStringField(deferredActor, e.off, row.value)) {
        if (!e.saidStamped) {
            e.saidStamped = true;
            UE_LOGI("[NQ] stamped %ls %ls", row.cls, row.prop);
        }
        return true;
    }
    ++g_failures;
    if (!e.saidFailed) {
        e.saidFailed = true;
        UE_LOGE("[NQ] stamp failed %ls %ls", row.cls, e.off < 0 ? e.missing : row.prop);
    }
    return false;
}

int FailureCount() { return g_failures; }

}  // namespace ue_wrap::spawner_stamp
