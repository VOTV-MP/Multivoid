// coop/dev/director/input_stub.cpp -- see coop/dev/director/input_stub.h.

#include "coop/dev/director/input_stub.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>
#include <vector>

namespace coop::director {
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// The stub's function, resolved once and said once when it does not resolve.
bool Resolved(InputStub& stub) {
    if (!stub.tried) {
        stub.tried = true;
        void* cls = R::FindClass(P::name::MainPlayerClass);
        stub.fn = cls ? R::FindFunction(cls, stub.name) : nullptr;
        if (!stub.fn) UE_LOGW("director: the input stub '%ls' does not resolve -- that key is never pressed", stub.name);
    }
    return stub.fn != nullptr;
}

}  // namespace

bool PressInputEvent(void* player, InputStub& stub) {
    if (!Resolved(stub) || !player) return false;
    const int32_t fs = R::FunctionFrameSize(stub.fn);
    std::vector<uint8_t> f(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
    return R::CallFunction(player, stub.fn, f.empty() ? nullptr : f.data());
}

bool PressKeyEvent(void* player, InputStub& stub, const R::FName& key) {
    if (!Resolved(stub) || !player) return false;
    ue_wrap::ParamFrame f(stub.fn);
    // An FKey is its name first (passwordlock's CallPlayerAnykey fills its key the same way).
    return f.valid() && f.SetRaw(L"Key", &key, static_cast<int32_t>(sizeof(key))) && ue_wrap::Call(player, f);
}

}  // namespace coop::director
