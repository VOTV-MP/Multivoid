// ue_wrap/desk/signal_dynamic.cpp -- see header.

#include "ue_wrap/desk/signal_dynamic.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/reflection.h"

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

// A leaf as the FName the struct stores: NAME_None for an empty leaf, and false for a non-empty one
// the engine interns to NAME_None ("None" in any case), which no reader could tell from empty.
bool LeafToFName(const std::wstring& leaf, R::FName& out) {
    out = R::FName{};
    if (leaf.empty()) return true;
    out = ue_wrap::fname_utils::StringToFName(leaf);
    return !(out.ComparisonIndex == 0 && out.Number == 0);
}

// A photo past this is no photo the laptop took (the local saves hold at most 8.7 KB): the array is
// read as garbage and left out.
constexpr int32_t kImageSanity = 4 * 1024 * 1024;

// Fstruct_byteImage: its one member, the photo's TArray<uint8>.
struct ByteArray {
    uint8_t* data;
    int32_t  num;
    int32_t  max;
};

}  // namespace

bool ReadStruct(const void* base, Row& out, bool withImage) {
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
    out.image.clear();
    if (withImage) {
        ByteArray img{};
        std::memcpy(&img, p + kOff_image, sizeof(img));
        if (img.data && img.num > 0 && img.num <= kImageSanity) out.image.assign(img.data, img.data + img.num);
    }
    return true;
}

bool WriteStructLive(void* base, const Row& in) {
    if (!base) return false;
    uint8_t* p = static_cast<uint8_t*>(base);
    // Every step that can fail runs first, into locals, and the struct is written only once all of
    // them have succeeded. Written in place, a failure stopped half way: a drive kept a new name and
    // id over its old signal, and no caller could tell which row it now held.
    R::FName object{}, signal{};
    if (!LeafToFName(in.object, object) || !LeafToFName(in.signal, signal)) return false;
    uint8_t nameHeader[sizeof(R::FString)] = {};
    uint8_t idHeader[sizeof(R::FString)] = {};
    // A name minted before a failed id is left unreferenced, as every replaced string is (header).
    if (!ue_wrap::fstring_utils::MintFString(in.name, nameHeader)) return false;
    if (!ue_wrap::fstring_utils::MintFString(in.id, idHeader)) return false;
    std::memcpy(p + kOff_name, nameHeader, sizeof(nameHeader));
    std::memcpy(p + kOff_id, idHeader, sizeof(idHeader));
    std::memcpy(p + kOff_object, &object, sizeof(object));
    std::memcpy(p + kOff_signal, &signal, sizeof(signal));
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
    R::FName object{}, signal{};
    if (!LeafToFName(in.object, object) || !LeafToFName(in.signal, signal)) return false;
    std::memcpy(out + kOff_object, &object, sizeof(object));
    std::memcpy(out + kOff_signal, &signal, sizeof(signal));
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
    if (!in.image.empty()) {
        ByteArray img{};
        img.data = const_cast<uint8_t*>(in.image.data());
        img.num = static_cast<int32_t>(in.image.size());
        img.max = img.num;
        std::memcpy(out + kOff_image, &img, sizeof(img));
    }
    return true;
}

}  // namespace ue_wrap::signal_dynamic
