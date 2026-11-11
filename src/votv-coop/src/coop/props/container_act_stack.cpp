// coop/props/container_act_stack.cpp -- the seats for what the inventory screen does to a stack in a shared inventory (a
// world container, or the drone's cargo) without the inventory's removeInventoryItem: taking part of a stack, and folding
// one entry into another.
//
// Part of a stack: dragging with the right button or the stack key down runs, in the screen's beginDrag, the item widget's
// takeFromStack(sub), which lowers the entry's amount in the inventory's array in place, then files a copy of the entry
// with the amount taken in the bag. The seat asks the host for the part before the body runs; the copy is found once
// beginDrag has filed it (its post), and a refusal takes it out of the bag again.
//
// A merge: the canvas's combine-stacks action runs, for two entries of one inventory, the widget's addToStack on the one
// that stays, then removeItem on the other. addToStack raises the amount in place and reports whether it could. The seat
// waits for the body to say it did (the amount rose by what was added), then asks the host to fold the one entry into the
// other; the removal that follows is the merge's own, not a take (container_act_request: NoteMergeSource). The host's
// merge is one write with nothing held, so no commit follows its grant.
// Gameplay layer (principle 7). Game thread.

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

#include "coop/interactables/atv_trunk_sync.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_target.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"

#include <cstring>
#include <vector>

namespace coop::props::container_act_request::detail {
namespace {

namespace caw = coop::props::container_act_wire;
namespace ci  = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace csw = coop::props::container_slice_wire;
namespace cv  = ue_wrap::container_view;
namespace R   = ue_wrap::reflection;
namespace II  = ue_wrap::inventory_item;
namespace sg  = ue_wrap::script_gate;
using coop::net::ContainerActFamily;

UE_FACT_FN(kRowTakeFromStack, "container_act", L"uicomp_FLI_item_C", L"takeFromStack", Declared);
UE_FACT_FN(kRowAddToStack,    "container_act", L"uicomp_FLI_item_C", L"addToStack", Declared);
UE_FACT_FN(kRowBeginDrag,     "container_act", L"ui_FreeLayoutInventory_C", L"beginDrag", Declared);
constexpr int kTagTakeFromStack = 0x43414335;   // "CAC5"
constexpr int kTagAddToStack    = 0x43414336;   // "CAC6"
constexpr int kTagBeginDrag     = 0x43414337;   // "CAC7"

bool g_said = false;

sg::Verdict Unreadable(const char* what) {
    if (!g_said) {
        g_said = true;
        UE_LOGW("container_act: the stack seats could not read %s -- a stack act in a shared inventory runs unasked", what);
    }
    return sg::Verdict::Run;
}

// Is the widget's inventory one the host shares? Fills the inventory, the target and the entries the widget's index names.
struct Shared {
    void* inventory = nullptr;
    uint32_t eid = 0;
    cv::WidgetEntry widget;
    std::vector<II::Item> records;
};

enum class Seat : uint8_t { Run, Cancel, Ask };

Seat ReadShared(void* widget, Shared& out) {
    coop::net::Session* const s = LinkedSession();
    if (!s || s->role() == coop::net::Role::Host) return Seat::Run;
    if (!cv::ReadWidget(widget, out.widget) || !out.widget.owner) return Seat::Run;
    out.inventory = out.widget.owner;
    if (!ue_wrap::drone::IsCargo(out.inventory) && !ci::IsWorldInventory(out.inventory) &&
        !coop::atv_trunk_sync::EidOfInventory(out.inventory))
        return Seat::Run;
    switch (SharedTargetOfInventory(out.inventory, out.eid)) {
    case Target::Own:
        return Seat::Run;
    case Target::NotReady:
        ShowNotReady();
        return Seat::Cancel;
    case Target::Shared:
        break;
    }
    if (!cio::ReadRecords(out.inventory, out.records)) {
        Unreadable("the inventory's records");
        return Seat::Run;
    }
    return Seat::Ask;
}

// The same item, wherever it lies on its canvas.
bool SameItem(const II::Item& a, const II::Item& b) {
    return a.classPath == b.classPath && a.name == b.name && a.tag == b.tag && a.key == b.key &&
           a.attachKey == b.attachKey && a.actorData == b.actorData && a.amount == b.amount;
}

// takeFromStack(sub): the part is asked of the host before the body lowers the entry.
sg::Verdict PreTakeFromStack(const sg::Call& c) {
    Shared sh;
    switch (ReadShared(c.object, sh)) {
    case Seat::Run:    return sg::Verdict::Run;
    case Seat::Cancel: return sg::Verdict::Cancel;
    case Seat::Ask:    break;
    }
    static R::ParamOffset sSub{L"sub"};
    const int32_t subOff = sSub.Of(c.function);
    if (subOff < 0 || !c.locals) return Unreadable("takeFromStack's parameter");
    const uint8_t sub = c.locals[subOff];
    const int32_t index = sh.widget.index;
    if (index < 0 || static_cast<size_t>(index) >= sh.records.size() || II::IsEmpty(sh.records[static_cast<size_t>(index)]))
        return sg::Verdict::Run;
    const II::Item& entry = sh.records[static_cast<size_t>(index)];
    if (entry.amount <= 1) return sg::Verdict::Run;   // the body takes nothing from a single unit
    // The body subtracts what it is given; a part that is not a part of the stack is a call the game never makes.
    if (sub == 0 || sub >= entry.amount) return sg::Verdict::Cancel;
    caw::Request r;
    r.family = static_cast<uint8_t>(ContainerActFamily::TakeStack);
    r.eid = sh.eid;
    r.index = static_cast<uint16_t>(index);
    r.hash = csw::RecordHash(entry);
    r.amount = sub;
    Take t;
    t.awaitCopy = true;
    t.from.Set(sh.inventory);
    t.part = entry;
    t.part.amount = sub;
    return AskTake(r, std::move(t)) ? sg::Verdict::Run : sg::Verdict::Cancel;
}

// The merge the body of addToStack is about to try: the entry that stays, the entry that is folded in and the amount.
struct MergeTry {
    bool open = false;
    ue_wrap::CachedObjRef widget;
    ue_wrap::CachedObjRef inventory;
    uint32_t eid = 0;
    uint16_t index = 0, indexB = 0;
    uint64_t hash = 0, hashB = 0;
    uint8_t amountBefore = 0, add = 0;
};
MergeTry g_try;

sg::Verdict PreAddToStack(const sg::Call& c) {
    g_try.open = false;
    Shared sh;
    switch (ReadShared(c.object, sh)) {
    case Seat::Run:    return sg::Verdict::Run;
    case Seat::Cancel: return sg::Verdict::Cancel;
    case Seat::Ask:    break;
    }
    static R::ParamOffset sAdd{L"add"};
    static R::ParamOffset sData{L"itemData"};
    const int32_t addOff = sAdd.Of(c.function);
    const int32_t dataOff = sData.Of(c.function);
    II::Item from;
    if (addOff < 0 || dataOff < 0 || !c.locals || !II::Read(c.locals + dataOff, from))
        return Unreadable("addToStack's parameters");
    const int32_t index = sh.widget.index;
    if (index < 0 || static_cast<size_t>(index) >= sh.records.size()) return sg::Verdict::Run;
    int32_t indexB = -1;
    for (size_t i = 0; i < sh.records.size() && indexB < 0; ++i)
        if (static_cast<int32_t>(i) != index && !II::IsEmpty(sh.records[i]) && SameItem(sh.records[i], from))
            indexB = static_cast<int32_t>(i);
    if (indexB < 0) {
        // The entry added is not one of this inventory's: nothing the host can fold, and the body is not let to raise a
        // stack the host will not.
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    g_try.open = true;
    g_try.widget.Set(c.object);
    g_try.inventory.Set(sh.inventory);
    g_try.eid = sh.eid;
    g_try.index = static_cast<uint16_t>(index);
    g_try.indexB = static_cast<uint16_t>(indexB);
    g_try.hash = csw::RecordHash(sh.records[static_cast<size_t>(index)]);
    g_try.hashB = csw::RecordHash(sh.records[static_cast<size_t>(indexB)]);
    g_try.amountBefore = sh.records[static_cast<size_t>(index)].amount;
    g_try.add = c.locals[addOff];
    return sg::Verdict::Run;
}

// The body has run: when it raised the stack by what was added, the merge happened here and is asked of the host.
void PostAddToStack(const sg::Call&) {
    if (!g_try.open) return;
    g_try.open = false;
    cv::WidgetEntry now;
    if (!cv::ReadWidget(g_try.widget.Get(), now) ||
        now.item.amount != static_cast<uint8_t>(g_try.amountBefore + g_try.add) || g_try.add == 0)
        return;   // the body refused: nothing changed here
    caw::Request r;
    r.family = static_cast<uint8_t>(ContainerActFamily::MergeStack);
    r.eid = g_try.eid;
    r.index = g_try.index;
    r.hash = g_try.hash;
    r.indexB = g_try.indexB;
    r.hashB = g_try.hashB;
    // The removal of the folded entry follows in the caller's body. Asked, it is the merge's; not asked, it is refused so
    // the folded entry is not taken without the merge, and the host's next publication puts the stack right.
    NoteMergeSource(g_try.inventory.Get(), g_try.hashB, AskTake(r, Take{}));
}

void PostBeginDrag(const sg::Call&) { AttachCopies(); }

}  // namespace

const WatchRow* StackWatches(size_t& count) {
    static const WatchRow kRows[] = {
        {kRowTakeFromStack, kTagTakeFromStack, &PreTakeFromStack, nullptr},
        {kRowAddToStack, kTagAddToStack, &PreAddToStack, &PostAddToStack},
        {kRowBeginDrag, kTagBeginDrag, nullptr, &PostBeginDrag},
    };
    count = sizeof(kRows) / sizeof(kRows[0]);
    return kRows;
}

}  // namespace coop::props::container_act_request::detail
