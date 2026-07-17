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

    // A resolved UFunction. buf_ may be empty for a no-param function (valid: the
    // call passes a null params buffer). Param read/write paths guard buf_ size.
    bool valid() const { return fn_ != nullptr; }
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

    // Internal metadata cache entry shared across every ParamFrame for the
    // same UFunction*. Built lazily on first construction for a given fn,
    // then reused as a read-only pointer by subsequent ParamFrames -- so
    // steady-state ParamFrame construction allocates ONLY buf_ (the
    // per-call zeroed frame), not the offsets vector or per-param wstrings.
    // Audit fix 2026-05-29 D4-1: prior ctor allocated 2 vectors + per-param
    // wstrings on every call, churning malloc/free on the per-snapshot
    // Drive() / per-tick observer dispatch paths.
    struct Metadata {
        int32_t frameSize = -1;  // < 0 => malformed UFunction; ParamFrame stays invalid
        std::vector<std::pair<std::wstring, int32_t>> offsets;
    };

private:
    // Param name -> frame offset, walked from the UFunction's FProperty chain ONCE
    // (cached process-wide; see ParamFrame::Metadata above).
    int32_t OffsetOf(const wchar_t* name) const;

    void* fn_ = nullptr;
    const Metadata* meta_ = nullptr;
    std::vector<uint8_t> buf_;
};

// Invoke `frame` on `object` (reflection::CallFunction under the hood). OUT
// params / ReturnValue are written back into the frame, readable via Get().
// Game thread only.
bool Call(void* object, ParamFrame& frame);

}  // namespace ue_wrap
