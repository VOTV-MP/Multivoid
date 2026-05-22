#include "ue_wrap/call.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <cstring>

namespace ue_wrap {

ParamFrame::ParamFrame(void* function) : fn_(function) {
    if (!fn_) return;
    const int32_t frameSize = reflection::FunctionFrameSize(fn_);
    if (frameSize <= 0) {
        UE_LOGE("ParamFrame: function %p has frame size %d", fn_, frameSize);
        fn_ = nullptr;
        return;
    }
    buf_.assign(static_cast<size_t>(frameSize), 0);
}

bool ParamFrame::SetRaw(const wchar_t* name, const void* src, int32_t size) {
    if (!valid()) return false;
    const int32_t off = reflection::FindParamOffset(fn_, name);
    if (off < 0) {
        UE_LOGE("ParamFrame::Set: unknown param '%ls'", name);
        return false;
    }
    if (off + size > static_cast<int32_t>(buf_.size())) {
        UE_LOGE("ParamFrame::Set: param '%ls' off=%d size=%d overflows frame %zu",
                name, off, size, buf_.size());
        return false;
    }
    std::memcpy(buf_.data() + off, src, static_cast<size_t>(size));
    return true;
}

bool ParamFrame::GetRaw(const wchar_t* name, void* dst, int32_t size) const {
    if (fn_ == nullptr || buf_.empty()) return false;
    const int32_t off = reflection::FindParamOffset(fn_, name);
    if (off < 0) {
        UE_LOGE("ParamFrame::Get: unknown param '%ls'", name);
        return false;
    }
    if (off + size > static_cast<int32_t>(buf_.size())) {
        UE_LOGE("ParamFrame::Get: param '%ls' off=%d size=%d overflows frame %zu",
                name, off, size, buf_.size());
        return false;
    }
    std::memcpy(dst, buf_.data() + off, static_cast<size_t>(size));
    return true;
}

bool Call(void* object, ParamFrame& frame) {
    if (!frame.valid()) return false;
    return reflection::CallFunction(object, frame.function(), frame.data());
}

}  // namespace ue_wrap
