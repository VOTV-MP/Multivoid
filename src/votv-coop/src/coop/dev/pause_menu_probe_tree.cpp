// coop/dev/pause_menu_probe_tree.cpp -- the pause-menu probe's widget tree dump (M1) and the identity of a widget.
// See coop/dev/pause_menu_probe.h.

#include "pause_menu_probe_detail.h"

#include "ui/native_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/umg_build.h"

#include <cstdint>
#include <vector>

namespace coop::dev::pause_menu_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace U  = ue_wrap::umg;
namespace NS = ui::native_screen;

constexpr int kMaxTreeLines = 400;

// UWidget::GetVisibility: 0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible, 4 SelfHitTestInvisible; -1 when
// the call cannot be made.
int VisibilityOf(void* w) {
    static void* fn = nullptr;
    if (!fn) {
        void* c = R::FindClass(L"Widget");
        fn = c ? R::FindFunction(c, L"GetVisibility") : nullptr;
    }
    if (!fn) return -1;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(w, f)) return -1;
    return f.Get<uint8_t>(L"ReturnValue");
}

struct TreeCtx {
    int lines = 0;
    int cut = 0;
    void* panelCls = nullptr;
    void* userCls = nullptr;
    std::vector<Ident>* ids = nullptr;
};

void DumpNode(const char* tag, void* w, int depth, int maxDepth, TreeCtx& c) {
    if (!w || !R::IsLive(w)) return;
    if (c.lines >= kMaxTreeLines) { ++c.cut; return; }
    ++c.lines;
    if (c.ids) c.ids->push_back(IdentOf(w));
    void* cls = R::ClassOf(w);
    void* slot = NS::SlotOf(w);
    const bool user = c.userCls && R::IsDescendantOfAny(cls, &c.userCls, 1, 24);
    const bool panel = !user && c.panelCls && R::IsDescendantOfAny(cls, &c.panelCls, 1, 24);
    UE_LOGI("pmprobe %s %*s%ls '%ls' vis=%d slot=%ls%s", tag, depth * 2, "", R::ClassNameOf(w).c_str(),
            R::ToString(R::NameOf(w)).c_str(), VisibilityOf(w), slot ? R::ClassNameOf(slot).c_str() : L"-",
            user ? " [user widget: leaf]" : "");
    if (!panel || depth >= maxDepth) return;
    const int32_t n = U::ChildCount(w);
    for (int32_t i = 0; i < n; ++i) DumpNode(tag, U::ChildAt(w, i), depth + 1, maxDepth, c);
}

}  // namespace

Ident IdentOf(void* w) {
    Ident id;
    id.p = w;
    id.idx = w ? R::InternalIndexOf(w) : -1;
    id.serial = id.idx >= 0 ? R::AllocateSlotSerial(id.idx) : 0;
    return id;
}

void DumpTree(const char* tag, void* root, int maxDepth, std::vector<Ident>* ids) {
    TreeCtx c;
    c.panelCls = R::FindClass(L"PanelWidget");
    c.userCls = R::FindClass(L"UserWidget");
    c.ids = ids;
    DumpNode(tag, root, 0, maxDepth, c);
    UE_LOGI("pmprobe %s widgets=%d%s", tag, c.lines, c.cut ? " (cut at the line cap)" : "");
}

}  // namespace coop::dev::pause_menu_probe::detail
