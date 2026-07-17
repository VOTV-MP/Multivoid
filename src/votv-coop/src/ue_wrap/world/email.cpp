// ue_wrap/email.cpp -- see ue_wrap/email.h.

#include "ue_wrap/world/email.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::email {
namespace {

namespace R  = ue_wrap::reflection;
namespace FT = ue_wrap::ftext_utils;

// Fstruct_email member offsets (struct stride 0x50; phase-2 impl RE SS3.2 --
// member names are GUID-mangled in the cooked struct, dump offsets
// authoritative per the door_box precedent).
constexpr int32_t kEmail_new      = 0x00;  // bool
constexpr int32_t kEmail_pfp      = 0x08;  // UTexture2D*
constexpr int32_t kEmail_username = 0x10;  // enum byte
constexpr int32_t kEmail_date     = 0x14;  // FIntVector (re-stamped by addEmail)
constexpr int32_t kEmail_topic    = 0x20;  // FText (0x18)
constexpr int32_t kEmail_text     = 0x38;  // FText (0x18)
constexpr int32_t kEmailStride    = 0x50;

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

void* g_gamemodeCls = nullptr;
void* g_gamemode = nullptr;
int32_t g_gamemodeIdx = -1;
int32_t g_offSaveSlot = -1;   // mainGamemode_C::saveSlot
int32_t g_offEmails = -1;     // saveSlot_C::emails (@0x0118)
int32_t g_offLaptop = -1;     // mainGamemode_C::laptop (@0x0448, Uui_laptop_C*)
void* g_saveSlotCls = nullptr;
void* g_laptopCls = nullptr;
void* g_addEmailFn = nullptr; // mainGamemode_C::addEmail(Fstruct_email item)
void* g_delEmailFn = nullptr; // ui_laptop_C::delEmail(int32 Index)

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gamemodeCls) g_gamemodeCls = R::FindClass(L"mainGamemode_C");
    if (!g_gamemodeCls) return;
    if (g_offSaveSlot < 0) g_offSaveSlot = R::FindPropertyOffset(g_gamemodeCls, L"saveSlot");
    if (g_offLaptop < 0) g_offLaptop = R::FindPropertyOffset(g_gamemodeCls, L"laptop");
    if (!g_addEmailFn) g_addEmailFn = R::FindFunction(g_gamemodeCls, L"addEmail");
    if (!g_saveSlotCls) g_saveSlotCls = R::FindClass(L"saveSlot_C");
    if (g_saveSlotCls && g_offEmails < 0)
        g_offEmails = R::FindPropertyOffset(g_saveSlotCls, L"emails");
    if (!g_laptopCls) g_laptopCls = R::FindClass(L"ui_laptop_C");
    if (g_laptopCls && !g_delEmailFn)
        g_delEmailFn = R::FindFunction(g_laptopCls, L"delEmail");
    const bool core = g_offSaveSlot >= 0 && g_offEmails >= 0 && g_addEmailFn &&
                      g_offLaptop >= 0 && g_delEmailFn;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("ue_email: resolved (saveSlot=0x%X emails=0x%X laptop=0x%X addEmail=yes delEmail=yes)",
                g_offSaveSlot, g_offEmails, g_offLaptop);
    }
}

void* Gamemode() {
    if (g_gamemode && R::IsLiveByIndex(g_gamemode, g_gamemodeIdx)) return g_gamemode;
    g_gamemode = nullptr;
    if (!g_gamemodeCls) return nullptr;
    for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
        if (obj && R::IsLive(obj)) {
            g_gamemode = obj;
            g_gamemodeIdx = R::InternalIndexOf(obj);
            break;
        }
    }
    return g_gamemode;
}

TArrayView* Emails() {
    void* gm = Gamemode();
    if (!gm || g_offSaveSlot < 0 || g_offEmails < 0) return nullptr;
    void* slot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSaveSlot);
    if (!slot || !R::IsLive(slot)) return nullptr;
    return reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(slot) + g_offEmails);
}

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

int32_t EmailCount() {
    TArrayView* a = Emails();
    if (!a) return -1;
    if (a->num < 0 || a->num > 4096) return -1;  // sanity
    return a->num;
}

bool ReadRow(int32_t index, Row& out) {
    TArrayView* a = Emails();
    if (!a || index < 0 || index >= a->num) return false;
    uint8_t* rp = a->data + static_cast<size_t>(index) * kEmailStride;
    out.username = *(rp + kEmail_username);
    out.pfpLeaf.clear();
    void* pfp = *reinterpret_cast<void**>(rp + kEmail_pfp);
    if (pfp && R::IsLive(pfp)) out.pfpLeaf = R::ToString(R::NameOf(pfp));
    out.topic = FT::FTextToString(rp + kEmail_topic);
    out.text  = FT::FTextToString(rp + kEmail_text);
    return true;
}

bool ReadRowKey(int32_t index, RowKey& out) {
    TArrayView* a = Emails();
    if (!a || index < 0 || index >= a->num) return false;
    const uint8_t* rp = a->data + static_cast<size_t>(index) * kEmailStride;
    std::memcpy(&out.topicData, rp + kEmail_topic, sizeof(out.topicData));
    std::memcpy(&out.textData, rp + kEmail_text, sizeof(out.textData));
    std::memcpy(&out.pfp, rp + kEmail_pfp, sizeof(out.pfp));
    std::memcpy(&out.dateX, rp + kEmail_date, sizeof(out.dateX));
    out.username = *(rp + kEmail_username);
    return true;
}

bool DelEmail(int32_t index) {
    TArrayView* a = Emails();
    if (!a || index < 0 || index >= a->num) return false;
    void* gm = Gamemode();
    if (!gm || g_offLaptop < 0 || !g_delEmailFn) return false;
    void* laptop = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offLaptop);
    if (!laptop || !R::IsLive(laptop)) return false;
    ue_wrap::ParamFrame f(g_delEmailFn);
    if (!f.valid()) return false;
    if (!f.Set(L"Index", index)) return false;
    return ue_wrap::Call(laptop, f);
}

bool AddEmail(const Row& row) {
    void* gm = Gamemode();
    if (!gm || !g_addEmailFn) return false;
    ue_wrap::ParamFrame f(g_addEmailFn);
    if (!f.valid()) return false;
    // Hand-build the 0x50 Fstruct_email in place inside the frame's `item`
    // param: new=true, pfp resolved-or-null, username byte, date zeroed
    // (addEmail re-stamps it from the synced daynightCycle clock), topic/text
    // minted FTexts (the +1 mint refs deliberately leak; the receiving
    // Array_Add deep-copies with proper refs -- ftext_utils doctrine).
    uint8_t item[kEmailStride] = {};
    item[kEmail_new] = 1;
    if (!row.pfpLeaf.empty()) {
        void* pfp = R::FindObject(row.pfpLeaf.c_str(), L"Texture2D");
        std::memcpy(item + kEmail_pfp, &pfp, sizeof(pfp));  // null-safe (empty brush)
    }
    item[kEmail_username] = row.username;
    if (!FT::MintFText(row.topic.c_str(), item + kEmail_topic)) return false;
    if (!FT::MintFText(row.text.c_str(), item + kEmail_text)) return false;
    if (!f.SetRaw(L"item", item, kEmailStride)) return false;
    return ue_wrap::Call(gm, f);
}

}  // namespace ue_wrap::email
