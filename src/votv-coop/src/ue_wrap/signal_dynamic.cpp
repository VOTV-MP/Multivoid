// ue_wrap/signal_dynamic.cpp -- see header.

#include "ue_wrap/signal_dynamic.h"

#include "ue_wrap/fname_utils.h"
#include "ue_wrap/fstring_utils.h"
#include "ue_wrap/reflection.h"

#include <cstring>

namespace ue_wrap::signal_dynamic {

namespace R = ue_wrap::reflection;

namespace {

std::wstring ReadFString(const uint8_t* p) {
    R::FString s{};
    std::memcpy(&s, p, sizeof(s));
    if (!s.Data || s.Num <= 1) return {};
    return std::wstring(s.Data, static_cast<size_t>(s.Num - 1));
}

std::wstring ReadFNameLeaf(const uint8_t* p) {
    R::FName n{};
    std::memcpy(&n, p, sizeof(n));
    if (n.ComparisonIndex == 0 && n.Number == 0) return {};  // NAME_None
    return R::ToString(n);
}

bool WriteFNameField(uint8_t* p, const std::wstring& leaf) {
    R::FName n{};  // NAME_None for empty input
    if (!leaf.empty()) {
        n = ue_wrap::fname_utils::StringToFName(leaf);
        if (n.ComparisonIndex == 0 && n.Number == 0) return false;
    }
    std::memcpy(p, &n, sizeof(n));
    return true;
}

}  // namespace

bool ReadStruct(const void* base, Row& out) {
    if (!base) return false;
    const uint8_t* p = static_cast<const uint8_t*>(base);
    out.name   = ReadFString(p + kOff_name);
    out.id     = ReadFString(p + kOff_id);
    out.object = ReadFNameLeaf(p + kOff_object);
    out.signal = ReadFNameLeaf(p + kOff_signal);
    std::memcpy(&out.level, p + kOff_level, sizeof(out.level));
    std::memcpy(&out.polarity, p + kOff_polarity, sizeof(out.polarity));
    std::memcpy(&out.size, p + kOff_size, sizeof(out.size));
    std::memcpy(&out.decoded, p + kOff_decoded, sizeof(out.decoded));
    std::memcpy(&out.date, p + kOff_date, sizeof(out.date));
    out.isCopy = *(p + kOff_isCopy) != 0;
    std::memcpy(&out.locX, p + kOff_loc + 0, sizeof(float));
    std::memcpy(&out.locY, p + kOff_loc + 4, sizeof(float));
    out.frequency  = *(p + kOff_freq);
    out.quality    = *(p + kOff_qual);
    out.objectType = *(p + kOff_objType);
    std::memcpy(&out.downloadedAtQuality, p + kOff_daq, sizeof(float));
    out.hasData = out.size > 0.0f;
    return true;
}

bool WriteStructLive(void* base, const Row& in) {
    if (!base) return false;
    uint8_t* p = static_cast<uint8_t*>(base);
    if (!ue_wrap::fstring_utils::MintFString(in.name, p + kOff_name)) return false;
    if (!ue_wrap::fstring_utils::MintFString(in.id, p + kOff_id)) return false;
    if (!WriteFNameField(p + kOff_object, in.object)) return false;
    if (!WriteFNameField(p + kOff_signal, in.signal)) return false;
    std::memcpy(p + kOff_level, &in.level, sizeof(in.level));
    std::memcpy(p + kOff_polarity, &in.polarity, sizeof(in.polarity));
    std::memcpy(p + kOff_size, &in.size, sizeof(in.size));
    std::memcpy(p + kOff_decoded, &in.decoded, sizeof(in.decoded));
    std::memcpy(p + kOff_date, &in.date, sizeof(in.date));
    *(p + kOff_isCopy) = in.isCopy ? 1 : 0;
    std::memcpy(p + kOff_loc + 0, &in.locX, sizeof(float));
    std::memcpy(p + kOff_loc + 4, &in.locY, sizeof(float));
    *(p + kOff_freq) = in.frequency;
    *(p + kOff_qual) = in.quality;
    *(p + kOff_objType) = in.objectType;
    std::memcpy(p + kOff_daq, &in.downloadedAtQuality, sizeof(float));
    // Empty the image (count only; the buffer stays engine-owned) so a stale
    // photo never rides with a different signal's data.
    int32_t zero = 0;
    std::memcpy(p + kOff_image + 8, &zero, sizeof(zero));  // TArray.Num
    return true;
}

bool BuildParamBytes(const Row& in, uint8_t out[kStride]) {
    if (!out) return false;
    std::memset(out, 0, kStride);
    // FStrings point at the caller-held Row's buffers; the callee (saveSignal's
    // Array_Add) deep-copies with the engine allocator during the call.
    auto setStr = [&](int32_t off, const std::wstring& s) {
        if (s.empty()) return;  // null FString = valid empty
        R::FString v{};
        v.Data = const_cast<wchar_t*>(s.c_str());
        v.Num  = static_cast<int32_t>(s.size() + 1);
        v.Max  = v.Num;
        std::memcpy(out + off, &v, sizeof(v));
    };
    setStr(kOff_name, in.name);
    setStr(kOff_id, in.id);
    if (!WriteFNameField(out + kOff_object, in.object)) return false;
    if (!WriteFNameField(out + kOff_signal, in.signal)) return false;
    std::memcpy(out + kOff_level, &in.level, sizeof(in.level));
    std::memcpy(out + kOff_polarity, &in.polarity, sizeof(in.polarity));
    std::memcpy(out + kOff_size, &in.size, sizeof(in.size));
    std::memcpy(out + kOff_decoded, &in.decoded, sizeof(in.decoded));
    std::memcpy(out + kOff_date, &in.date, sizeof(in.date));
    out[kOff_isCopy] = in.isCopy ? 1 : 0;
    std::memcpy(out + kOff_loc + 0, &in.locX, sizeof(float));
    std::memcpy(out + kOff_loc + 4, &in.locY, sizeof(float));
    out[kOff_freq] = in.frequency;
    out[kOff_qual] = in.quality;
    out[kOff_objType] = in.objectType;
    std::memcpy(out + kOff_daq, &in.downloadedAtQuality, sizeof(float));
    // image stays the zeroed empty TArray.
    return true;
}

}  // namespace ue_wrap::signal_dynamic
