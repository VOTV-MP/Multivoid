#include "coop/interactables/notebook_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/props/prop_save_data.h"
#include "coop/net/session.h"
#include "coop/props/prop_key_index.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/desk/device_screen.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cstring>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace coop::notebook_sync {
namespace {
namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using coop::net::Role;

UE_FACT_FN(kNotebookGraph, "notebook_sync", L"ui_notebook_C", L"ExecuteUbergraph_ui_notebook", Declared);
UE_FACT_FN(kNotebookQuit, "notebook_sync", L"ui_notebook_C", L"quit", Declared);
std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_assembler;
uint32_t g_nextSeq = 1;
void* g_notebookBase = nullptr;
int32_t g_widgetClipboardOff = -1;
int32_t g_widgetMletTextOff = -1;
int32_t g_propPageOff = -1;
int32_t g_propTextOff = -1;
constexpr size_t kMaxTextBlob = 256 * 1024;
constexpr auto kAssemblyTTL = std::chrono::seconds(20);
std::chrono::steady_clock::time_point g_nextSweep{};
std::unordered_map<std::wstring, std::pair<uint8_t, std::chrono::steady_clock::time_point>> g_recentNotebookOwner;
constexpr auto kNotebookClaimGrace = std::chrono::seconds(3);

coop::net::Session* Session() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() ? s : nullptr;
}

void Put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
bool Get16(const std::vector<uint8_t>& b, size_t& p, uint16_t& v) {
    if (p + 2 > b.size()) return false;
    v = static_cast<uint16_t>(b[p] | (uint16_t(b[p + 1]) << 8)); p += 2; return true;
}
bool Get32(const std::vector<uint8_t>& b, size_t& p, uint32_t& v) {
    if (p + 4 > b.size()) return false;
    v = uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) |
        (uint32_t(b[p + 3]) << 24); p += 4; return true;
}

// The blob is a prop key followed by a UTF-16LE page count and length-prefixed UTF-16LE pages.
bool Encode(const std::wstring& key, const std::vector<std::wstring>& pages, std::vector<uint8_t>& out) {
    if (key.empty() || key.size() > 128 || pages.size() > 256) return false;
    out.clear(); Put16(out, static_cast<uint16_t>(key.size()));
    for (wchar_t c : key) Put16(out, static_cast<uint16_t>(c));
    Put16(out, static_cast<uint16_t>(pages.size()));
    for (const auto& page : pages) {
        if (page.size() > 32768 || out.size() + page.size() * 2 + 4 > kMaxTextBlob) return false;
        Put32(out, static_cast<uint32_t>(page.size()));
        for (wchar_t c : page) Put16(out, static_cast<uint16_t>(c));
    }
    return out.size() <= std::min(kMaxTextBlob, coop::blob_chunks::MaxBlobBytes());
}

bool Decode(const std::vector<uint8_t>& b, std::wstring& key, std::vector<std::wstring>& pages) {
    if (b.size() > kMaxTextBlob) return false;
    size_t p = 0; uint16_t keyLen = 0, count = 0;
    if (!Get16(b, p, keyLen) || keyLen == 0 || keyLen > 128 || p + size_t(keyLen) * 2 > b.size()) return false;
    key.clear(); for (uint16_t i = 0; i < keyLen; ++i) { uint16_t c; if (!Get16(b, p, c)) return false; key.push_back(static_cast<wchar_t>(c)); }
    if (!Get16(b, p, count) || count > 256) return false;
    pages.clear(); pages.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t len = 0; if (!Get32(b, p, len) || len > 32768 || p + size_t(len) * 2 > b.size()) return false;
        std::wstring page; page.reserve(len);
        for (uint32_t j = 0; j < len; ++j) { uint16_t c; if (!Get16(b, p, c)) return false; page.push_back(static_cast<wchar_t>(c)); }
        pages.push_back(std::move(page));
    }
    return p == b.size();
}

bool IsNotebook(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cls = R::ClassOf(actor);
    if (!cls) return false;
    if (!g_notebookBase) g_notebookBase = R::FindClass(L"prop_notebook_C");
    return g_notebookBase && R::IsDescendantOfAny(cls, &g_notebookBase, 1);
}

bool Apply(const std::vector<uint8_t>& blob) {
    std::wstring key; std::vector<std::wstring> pages;
    if (!Decode(blob, key, pages)) return false;
    void* actor = coop::prop_key_index::Find(key);
    if (!IsNotebook(actor)) return false;
    if (g_propTextOff < 0) g_propTextOff = R::FindPropertyOffset(R::ClassOf(actor), L"Text");
    return g_propTextOff >= 0 && ue_wrap::field_io::WriteFStringArrayField(actor, g_propTextOff, pages);
}

void Publish(coop::net::Session* s, const std::vector<uint8_t>& blob) {
    if (s->role() == Role::Host)
        coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::NotebookText, g_nextSeq++, blob);
    else
        coop::blob_chunks::SendBlobToSlot(s, 0, coop::net::ReliableKind::NotebookText, g_nextSeq++, blob);
}

void OnNotebookGraphPost(const sg::Call& c) {
    auto* s = Session(); if (!s || !c.object || !R::IsLive(c.object)) return;
    const std::wstring claim = ue_wrap::device_screen::ClassifyWidgetClaimKey(c.object);
    if (claim.empty()) return;
    if (g_widgetClipboardOff < 0) g_widgetClipboardOff = R::FindPropertyOffset(R::ClassOf(c.object), L"clipboard");
    if (g_widgetClipboardOff < 0) return;
    void* prop = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c.object) + g_widgetClipboardOff);
    if (!IsNotebook(prop)) return;
    const std::wstring key = ue_wrap::prop::GetKeyString(prop);
    if (key.empty()) return;
    if (g_widgetMletTextOff < 0) g_widgetMletTextOff = R::FindPropertyOffset(R::ClassOf(c.object), L"MLET_text");
    if (g_widgetMletTextOff < 0) return;
    void* editor = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c.object) + g_widgetMletTextOff);
    if (!editor || !R::IsLive(editor)) return;
    void* getText = R::FindDispatchFunctionCached(R::ClassOf(editor), L"GetText");
    if (!getText) return;
    ue_wrap::ParamFrame textFrame(getText);
    if (!textFrame.valid() || !ue_wrap::Call(editor, textFrame)) return;
    uint8_t ftext[ue_wrap::ftext_utils::kFTextSize]{};
    if (!textFrame.GetRaw(L"ReturnValue", ftext, sizeof(ftext))) return;
    const std::wstring currentPage = ue_wrap::ftext_utils::FTextToString(ftext);
    if (g_propTextOff < 0) g_propTextOff = R::FindPropertyOffset(R::ClassOf(prop), L"Text");
    if (g_propPageOff < 0) g_propPageOff = R::FindPropertyOffset(R::ClassOf(prop), L"page");
    if (g_propTextOff < 0 || g_propPageOff < 0) return;
    auto pages = ue_wrap::field_io::ReadFStringArrayField(prop, g_propTextOff);
    const int32_t page = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(prop) + g_propPageOff);
    if (page < 0 || static_cast<size_t>(page) >= pages.size()) return;
    // The widget text is newer than clipboard.Text until quit(), so fold the open page into the
    // backing array before publishing the full canonical snapshot.
    pages[static_cast<size_t>(page)] = currentPage;
    if (g_propTextOff < 0) g_propTextOff = R::FindPropertyOffset(R::ClassOf(prop), L"Text");
    if (g_propTextOff < 0 || !ue_wrap::field_io::WriteFStringArrayField(prop, g_propTextOff, pages)) return;
    std::vector<uint8_t> blob;
    if (!Encode(key, pages, blob)) return;
    Publish(s, blob);
}

void OnNotebookQuitPost(const sg::Call& c) {
    auto* s = Session();
    if (!s || !c.object || !R::IsLive(c.object)) return;
    if (g_widgetClipboardOff < 0)
        g_widgetClipboardOff = R::FindPropertyOffset(R::ClassOf(c.object), L"clipboard");
    if (g_widgetClipboardOff < 0) return;
    void* prop = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c.object) + g_widgetClipboardOff);
    if (!IsNotebook(prop)) return;
    const std::wstring key = ue_wrap::prop::GetKeyString(prop);
    if (key.empty() || key == L"None") return;
    // quit() committed the current editor contents to clipboard.Text. Send the complete page
    // array on the dedicated lane; PropSaveDataIntent is holder-gated and is not sufficient here.
    if (g_propTextOff < 0) g_propTextOff = R::FindPropertyOffset(R::ClassOf(prop), L"Text");
    if (g_propTextOff >= 0) {
        const auto pages = ue_wrap::field_io::ReadFStringArrayField(prop, g_propTextOff);
        std::vector<uint8_t> blob;
        if (Encode(key, pages, blob)) Publish(s, blob);
    }
    coop::prop_save_data::Publish(s, prop, key);
}

} // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    sg::WatchName(kNotebookGraph, 0, nullptr, &OnNotebookGraphPost);
    sg::WatchName(kNotebookQuit, 0, nullptr, &OnNotebookQuitPost);
}

void Tick() {
    const auto now = std::chrono::steady_clock::now();
    if (g_assembler.Idle() || now < g_nextSweep) return;
    g_nextSweep = now + std::chrono::seconds(1);
    g_assembler.Sweep(now, kAssemblyTTL);
}

void OnReliable(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    auto* s = Session(); if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_assembler.OnChunk(chunk, senderSlot, blob)) return;
    if (s->role() == Role::Host) {
        std::wstring key; std::vector<std::wstring> pages;
        if (!Decode(blob, key, pages)) return;
        // The source must still own this notebook's live claim.
        uint64_t h = 1469598103934665603ull;
        for (wchar_t c : key) { h ^= static_cast<uint16_t>(c); h *= 1099511628211ull; }
        wchar_t claimBuf[22]{}; swprintf_s(claimBuf, L"note_%016llx", static_cast<unsigned long long>(h));
        const auto now = std::chrono::steady_clock::now();
        const auto prior = g_recentNotebookOwner.find(claimBuf);
        const bool holder = coop::device_occupancy::HolderOf(claimBuf) == senderSlot;
        const bool grace = prior != g_recentNotebookOwner.end() && prior->second.first == senderSlot &&
                           now - prior->second.second <= kNotebookClaimGrace;
        if (!holder && !grace) return;
        if (!Apply(blob)) return;
        g_recentNotebookOwner[claimBuf] = {senderSlot, now};
        Publish(s, blob);
    } else {
        if (senderSlot != 0) return;
        if (!Apply(blob)) UE_LOGW("notebook_sync: host notebook text could not be applied");
    }
}

void OnDisconnectForSlot(int slot) {
    if (slot >= 0 && slot < coop::net::kMaxPeers)
        g_assembler.ClearSlot(static_cast<uint8_t>(slot));
}

void OnDisconnect() { g_session.store(nullptr, std::memory_order_release); g_assembler.Clear(); g_recentNotebookOwner.clear(); }

} // namespace coop::notebook_sync
