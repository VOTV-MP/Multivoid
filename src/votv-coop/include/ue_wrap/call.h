// ue_wrap/call.h -- build a UFunction parameter frame and invoke it.
//
// Engine-wrapper layer (principle 7). A UFunction call via ProcessEvent needs a
// parameter frame with each argument at the exact byte offset the engine
// expects. ParamFrame allocates a correctly-sized, zeroed frame and lets you set
// arguments BY NAME -- the offsets come from reflection (the live UFunction's
// FProperty chain), so this is correct-by-construction and version-portable
// (no hardcoded offsets). Must be invoked on the game thread.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ue_wrap {

class ParamFrame {
public:
    // `function` is a UFunction*. Allocates UFunction::PropertiesSize bytes,
    // zeroed (so unset params/out-params start clean).
    explicit ParamFrame(void* function);

    bool valid() const { return fn_ != nullptr && !buf_.empty(); }
    void* function() const { return fn_; }
    void* data() { return buf_.empty() ? nullptr : buf_.data(); }

    // Write `size` bytes from `src` at parameter `name`'s frame offset. Returns
    // false (and logs) if the param is unknown or would overflow the frame.
    bool SetRaw(const wchar_t* name, const void* src, int32_t size);

    // Read `size` bytes of parameter `name` (e.g. a ReturnValue / OUT param)
    // into `dst` after the call. Returns false if unknown/overflow.
    bool GetRaw(const wchar_t* name, void* dst, int32_t size) const;

    template <class T>
    bool Set(const wchar_t* name, const T& value) {
        return SetRaw(name, &value, static_cast<int32_t>(sizeof(T)));
    }
    template <class T>
    T Get(const wchar_t* name) const {
        T value{};
        GetRaw(name, &value, static_cast<int32_t>(sizeof(T)));
        return value;
    }

private:
    // Param name -> frame offset, walked from the UFunction's FProperty chain ONCE
    // in the ctor (not per Set/Get -- that re-walked the chain + heap-allocated a
    // vector on every argument, churning on the per-snapshot Drive() path).
    int32_t OffsetOf(const wchar_t* name) const;

    void* fn_ = nullptr;
    std::vector<uint8_t> buf_;
    std::vector<std::pair<std::wstring, int32_t>> offsets_;
};

// Invoke `frame` on `object` (reflection::CallFunction under the hood). OUT
// params / ReturnValue are written back into the frame, readable via Get().
// Game thread only.
bool Call(void* object, ParamFrame& frame);

}  // namespace ue_wrap
