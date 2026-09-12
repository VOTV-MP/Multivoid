// ue_wrap/core/uobject_listeners.cpp -- one create and one delete listener on the engine's
// FUObjectArray, and the queue they feed. The list offsets, the lock and the vtable shape are the
// engine's, taken from RE-UE4SS's 4.27 member layout
// (reference/RE-UE4SS/assets/MemberVarLayoutTemplates/MemberVariableLayout_4_27_Template.ini)
// and confirmed against the shipping binary.
#include "ue_wrap/core/uobject_listeners.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ue_wrap::uobject_listeners {
namespace {

namespace O = profile::off;

// The engine's listener interfaces, laid out slot for slot: a deleting destructor the engine never
// calls, the notification, the shutdown call. `this` arrives in rcx as for any member function.
struct CreateListener {
    virtual ~CreateListener() = default;
    virtual void NotifyUObjectCreated(const void* object, int32_t index) = 0;
    virtual void OnUObjectArrayShutdown() = 0;
};
struct DeleteListener {
    virtual ~DeleteListener() = default;
    virtual void NotifyUObjectDeleted(const void* object, int32_t index) = 0;
    virtual void OnUObjectArrayShutdown() = 0;
};

std::atomic<bool>     g_armed{false};
bool                  g_installed = false;  // boot thread, before any reader
std::mutex            g_mu;
std::vector<Event>    g_queue;    // under g_mu
std::vector<Event>    g_scratch;  // the drain's side of the swap; game thread only
size_t                g_highWater = 0;  // under g_mu
std::atomic<uint64_t> g_created{0};
std::atomic<uint64_t> g_deleted{0};
// The RED calibration of the index parity drill: every n-th create notify is dropped so the drill
// can be seen to fail. Read once at Install; 0 in every ordinary run.
uint32_t              g_dropEveryNth = 0;
std::atomic<uint32_t> g_createSeq{0};

void Record(const void* object, int32_t index, bool created) {
    // A birth carries its class, set by the constructor before the slot is taken; a death names
    // only the slot, which the index already knows the class of, so nothing of the dying object is
    // read -- the shape of MTA's destructor handlers (CClientGame::GameObjectDestructHandler in
    // reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp), which treat the pointer as
    // an opaque key. MTA needs no birth notification because it spawns every entity itself; this
    // game spawns on its own, so the birth side reads the one field the seam guarantees.
    void* obj = const_cast<void*>(object);
    void* cls = created ? reflection::ClassOf(obj) : nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.push_back(Event{obj, cls, index, created});
    if (g_queue.size() > g_highWater) g_highWater = g_queue.size();
}

// TArray<T*> = {T** Data; int32 Num; int32 Max}, addressed by the array's base.
void**&  Data(uintptr_t t) { return *reinterpret_cast<void***>(t + O::TArray_Data); }
int32_t& Num(uintptr_t t)  { return *reinterpret_cast<int32_t*>(t + O::TArray_Num); }
int32_t& Max(uintptr_t t)  { return *reinterpret_cast<int32_t*>(t + O::TArray_Max); }

// Append with the store order a lock-free reader tolerates. The engine's create loop compares Num
// before it loads Data on every iteration, so publishing Data (and Max) before Num means a reader
// that sees the new count sees the new buffer. A buffer that had to be replaced is retired, never
// freed, when no lock covers the readers: one that loaded the old pointer an instruction earlier
// is still indexing it, and nothing says when it is done. The engine frees whatever buffer the
// array holds at its own shutdown; ours comes from its heap for exactly that reason.
bool Append(uintptr_t t, void* listener, bool readersLocked) {
    void** data = Data(t);
    const int32_t num = Num(t);
    const int32_t max = Max(t);
    if (num < max) {
        data[num] = listener;
        std::atomic_thread_fence(std::memory_order_release);
        Num(t) = num + 1;
        return true;
    }
    const int32_t grown = num + 8;
    void** fresh = static_cast<void**>(reflection::EngineAlloc(sizeof(void*) * grown));
    if (!fresh) return false;
    if (num > 0) std::memcpy(fresh, data, sizeof(void*) * num);
    fresh[num] = listener;
    std::atomic_thread_fence(std::memory_order_release);
    Data(t) = fresh;
    Max(t) = grown;
    std::atomic_thread_fence(std::memory_order_release);
    Num(t) = num + 1;
    if (data && readersLocked) reflection::EngineFree(data);
    return true;
}

// The engine's own removal: the last entry into the hole, then the count down.
void RemoveAtSwap(uintptr_t t, void* listener) {
    void** data = Data(t);
    const int32_t num = Num(t);
    for (int32_t i = 0; i < num; ++i) {
        if (data[i] != listener) continue;
        data[i] = data[num - 1];
        std::atomic_thread_fence(std::memory_order_release);
        Num(t) = num - 1;
        return;
    }
}

struct OurCreateListener final : CreateListener {
    void NotifyUObjectCreated(const void* object, int32_t index) override {
        if (!g_armed.load(std::memory_order_acquire)) return;
        if (g_dropEveryNth != 0 &&
            (g_createSeq.fetch_add(1, std::memory_order_relaxed) % g_dropEveryNth) == 0) return;
        g_created.fetch_add(1, std::memory_order_relaxed);
        Record(object, index, true);
    }
    void OnUObjectArrayShutdown() override {
        // The engine fatal-logs a listener still registered after this call.
        g_armed.store(false, std::memory_order_relaxed);
        RemoveAtSwap(reflection::ObjectArrayAddress() + O::FUObjectArray_CreateListeners, this);
        UE_LOGI("uobject_listeners: create listener removed at the engine's shutdown");
    }
};

struct OurDeleteListener final : DeleteListener {
    void NotifyUObjectDeleted(const void* object, int32_t index) override {
        if (!g_armed.load(std::memory_order_acquire)) return;
        g_deleted.fetch_add(1, std::memory_order_relaxed);
        Record(object, index, false);
    }
    void OnUObjectArrayShutdown() override {
        // Called under the engine's lock, which is recursive, so the removal may take it again.
        g_armed.store(false, std::memory_order_relaxed);
        RemoveAtSwap(reflection::ObjectArrayAddress() + O::FUObjectArray_DeleteListeners, this);
        UE_LOGI("uobject_listeners: delete listener removed at the engine's shutdown");
    }
};

OurCreateListener g_createListener;
OurDeleteListener g_deleteListener;

// The layout check before the first write: both lists must read as small, consistent TArrays and
// the lock as a critical section nobody holds. A profile that no longer matches the engine fails
// here, disarmed and logged, rather than by writing a pointer into whatever now lives there.
bool LayoutLooksRight(uintptr_t arr) {
    for (size_t list : {O::FUObjectArray_CreateListeners, O::FUObjectArray_DeleteListeners}) {
        const uintptr_t t = arr + list;
        const int32_t num = Num(t), max = Max(t);
        const bool sane = num >= 0 && max >= num && max <= 256 && ((max == 0) == (Data(t) == nullptr));
        if (!sane) {
            UE_LOGE("uobject_listeners: listener list at +0x%zX does not read as a TArray (num=%d max=%d data=%p)",
                    list, num, max, static_cast<void*>(Data(t)));
            return false;
        }
    }
    const auto* lock = reinterpret_cast<const CRITICAL_SECTION*>(arr + O::FUObjectArray_DeleteListenersCritical);
    if (lock->RecursionCount < 0 || lock->RecursionCount > 64) {
        UE_LOGE("uobject_listeners: the delete-list lock does not read as a critical section (recursion=%ld)",
                lock->RecursionCount);
        return false;
    }
    return true;
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    const uintptr_t arr = reflection::ObjectArrayAddress();
    if (!arr) return false;
    if (!LayoutLooksRight(arr)) return false;
    {
        char v[16] = {};
        if (::GetEnvironmentVariableA("VOTVCOOP_LISTENER_DROP_CREATES", v, sizeof(v)) > 0)
            g_dropEveryNth = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (g_dropEveryNth != 0)
            UE_LOGW("uobject_listeners: [drill] dropping every %u-th create notify (RED calibration)",
                    g_dropEveryNth);
    }
    g_queue.reserve(1 << 16);
    g_scratch.reserve(1 << 16);
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(arr + O::FUObjectArray_DeleteListenersCritical);
    ::EnterCriticalSection(lock);
    const bool deleteOk = Append(arr + O::FUObjectArray_DeleteListeners, &g_deleteListener, true);
    ::LeaveCriticalSection(lock);
    if (!deleteOk) {
        UE_LOGE("uobject_listeners: could not append the delete listener (engine heap unresolved?)");
        return false;
    }
    if (!Append(arr + O::FUObjectArray_CreateListeners, &g_createListener, false)) {
        UE_LOGE("uobject_listeners: could not append the create listener; the delete one stays "
                "registered and disarmed");
        return false;
    }
    g_armed.store(true, std::memory_order_release);
    g_installed = true;
    UE_LOGI("uobject_listeners: registered with the engine (create list %d/%d, delete list %d/%d)",
            Num(arr + O::FUObjectArray_CreateListeners), Max(arr + O::FUObjectArray_CreateListeners),
            Num(arr + O::FUObjectArray_DeleteListeners), Max(arr + O::FUObjectArray_DeleteListeners));
    return true;
}

void Uninstall() {
    g_armed.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.clear();
}

size_t Drain(std::vector<Event>& out) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_queue.empty()) return 0;
        g_queue.swap(g_scratch);
    }
    const size_t n = g_scratch.size();
    out.insert(out.end(), g_scratch.begin(), g_scratch.end());
    g_scratch.clear();
    return n;
}

Stats GetStats() {
    Stats s{};
    s.created = g_created.load(std::memory_order_relaxed);
    s.deleted = g_deleted.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mu);
    s.highWater = g_highWater;
    return s;
}

}  // namespace ue_wrap::uobject_listeners
