// ue_wrap/core/reflection_call.cpp -- the one path our code calls a UFunction through:
// CallFunction and what it keeps per call, the coop-origin latch, the call census and the frame
// that says whether the dispatch it entered faulted. Declared in reflection.h; which body an
// instance would run is reflection_dispatch.cpp's question.

#include "ue_wrap/core/reflection.h"

#include <atomic>

namespace ue_wrap::reflection {

// The coop-origin dispatch latch: every dispatch our code issues goes through this one choke
// point, so a thread-local depth tells "the mod called this native" from "the game's own
// blueprint called it" inside an interceptor; the context object alone cannot, since our
// re-arms set timers on game objects. A depth, not a bool, so a nested dispatch stays tagged;
// RAII, so an unwind cannot leave it stuck.
namespace {
thread_local int t_coopDispatchDepth = 0;
struct CoopDispatchScope {
    CoopDispatchScope() { ++t_coopDispatchDepth; }
    ~CoopDispatchScope() { --t_coopDispatchDepth; }
};
}  // namespace

bool InCoopDispatch() { return t_coopDispatchDepth > 0; }

// Coop-call attribution: which reflected calls make up the blueprint dispatches per frame our
// code authors, keyed on the target UFunction, since that is what names the polling (a
// per-tick location poll shows as one function at dozens per frame) with no stack walk.
// Counts only when armed, so shipping pays one relaxed load. Here and not in the ProcessEvent
// detour: this choke point runs at a small fraction of the detour's rate and is our own code
// rather than the engine's hottest path, where an instrument in the unprotected outer frame
// once crashed the game.
namespace {
constexpr int kCallSites = 128;
std::atomic<bool> g_callCensusOn{false};
// The plain call total, counted whether or not the census above is armed: the attribution needs
// arming because resolving names is expensive, while the rate is one relaxed add on a path that
// is about to dispatch a whole blueprint body, and a rate nobody thought to arm for is exactly
// the one a field report leaves behind.
std::atomic<unsigned long long> g_coopCalls{0};
struct CallSite {
    std::atomic<void*> fn{nullptr};
    std::atomic<unsigned long long> n{0};
};
CallSite g_callSites[kCallSites];

void NoteCoopCall(void* fn) {
    for (int i = 0; i < kCallSites; ++i) {
        void* cur = g_callSites[i].fn.load(std::memory_order_relaxed);
        if (cur == fn) { g_callSites[i].n.fetch_add(1, std::memory_order_relaxed); return; }
        if (!cur) {
            void* expected = nullptr;
            if (g_callSites[i].fn.compare_exchange_strong(expected, fn,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                g_callSites[i].n.fetch_add(1, std::memory_order_relaxed);
            }
            // Whether the slot was won or lost, the next pass re-reads it; losing a race must not
            // drop the sample.
            --i;
        }
    }
}
}  // namespace

void SetCoopCallCensus(bool on) { g_callCensusOn.store(on, std::memory_order_relaxed); }

unsigned long long CoopCallCountTotal() { return g_coopCalls.load(std::memory_order_relaxed); }

bool CoopCallSiteAt(int i, void** outFn, unsigned long long* outCount) {
    if (i < 0 || i >= kCallSites || !outFn || !outCount) return false;
    *outFn = g_callSites[i].fn.load(std::memory_order_relaxed);
    *outCount = g_callSites[i].n.load(std::memory_order_relaxed);
    return *outFn != nullptr;
}

namespace {
// The innermost CallFunction on this thread and whether a dispatch with its object and function
// faulted under it; the frame lives on CallFunction's stack, and a scope restores the one it hid.
struct CallFrame { void* object; void* function; bool faulted; };
thread_local CallFrame* t_call = nullptr;
struct CallFrameScope {
    CallFrame* hidden;
    explicit CallFrameScope(CallFrame* f) : hidden(t_call) { t_call = f; }
    ~CallFrameScope() { t_call = hidden; }
};
}  // namespace

void NoteDispatchFault(void* object, void* function) {
    if (t_call && t_call->object == object && t_call->function == function) t_call->faulted = true;
}

bool CallFunction(void* object, void* function, void* params) {
    using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);
    const auto processEvent = reinterpret_cast<ProcessEventFn>(ProcessEventAddr());
    if (!processEvent || !object || !function) return false;
    g_coopCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_callCensusOn.load(std::memory_order_relaxed)) NoteCoopCall(function);
    CallFrame frame{object, function, false};
    const CallFrameScope frameScope(&frame);
    CoopDispatchScope scope;
    processEvent(object, function, params);
    return !frame.faulted;
}

}  // namespace ue_wrap::reflection
