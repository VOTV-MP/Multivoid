// coop/props/prop_record_gates.cpp -- see coop/props/prop_record_gates.h.

#include "coop/props/prop_record_gates.h"

#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cwchar>
#include <vector>

namespace coop::prop_record_gates {
namespace {

namespace R = ue_wrap::reflection;

template <class Fn>
struct Row {
    const wchar_t* className;
    Fn fn;
};
std::vector<Row<IntentGate>> g_intent;
std::vector<Row<IntentRecordGate>> g_intentRecord;
std::vector<Row<ApplyGate>> g_apply;

template <class Fn>
void Add(std::vector<Row<Fn>>& rows, const wchar_t* className, Fn fn) {
    if (!className || !fn) return;
    for (const Row<Fn>& r : rows)
        if (r.fn == fn && std::wcscmp(r.className, className) == 0) return;
    rows.push_back({className, fn});
}

// Is the actor of the row's class or below? One index lookup and a short walk up the actor's class chain.
bool Names(const wchar_t* className, void* actor) {
    void* base = ue_wrap::object_index::ClassByName(className);
    void* cls = R::ClassOf(actor);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

}  // namespace

void RegisterIntentGate(const wchar_t* className, IntentGate fn) { Add(g_intent, className, fn); }
void RegisterIntentRecordGate(const wchar_t* className, IntentRecordGate fn) { Add(g_intentRecord, className, fn); }
void RegisterApplyGate(const wchar_t* className, ApplyGate fn) { Add(g_apply, className, fn); }

bool IntentAllows(void* actor, uint8_t senderSlot) {
    for (const Row<IntentGate>& r : g_intent)
        if (Names(r.className, actor) && !r.fn(actor, senderSlot)) return false;
    return true;
}

bool IntentRecordAllows(void* actor, uint8_t senderSlot, const ue_wrap::save_record::SaveRecord& rec) {
    for (const Row<IntentRecordGate>& r : g_intentRecord)
        if (Names(r.className, actor) && !r.fn(actor, senderSlot, rec)) return false;
    return true;
}

bool ApplyAllows(void* actor) {
    for (const Row<ApplyGate>& r : g_apply)
        if (Names(r.className, actor) && !r.fn(actor)) return false;
    return true;
}

}  // namespace coop::prop_record_gates
