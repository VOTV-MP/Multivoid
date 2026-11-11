// coop/props/container_act_drag.cpp -- the put seat for a drag onto the inventory screen's canvas, and its replay. Dropping
// an entry on the canvas of a shared inventory (a world container, or the drone's cargo) runs, in the screen's enddrag or
// clickoff, the inventory's addItemAdvanced with the entry of the bag's item widget, then the widget's removeItem. The seat
// cancels the addItemAdvanced, which the screen reads as a drop that did not fit (the widget stays in the bag), and asks the
// host with the entry as the inventory would store it. On a grant the verb runs again under the replay latch if the widget
// still holds that item in the bag; the widget is taken out of the bag only once the inventory has stored the entry, and
// the commit says where it was stored. A refusal needs no undoing: the entry never left the bag.
// The pending puts and the commit queue are container_act_request's (see container_act_request_internal.h). Gameplay layer
// (principle 7). Game thread.

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

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
#include "ue_wrap/facts/function_row.h"

#include <cstring>
#include <optional>
#include <vector>

namespace coop::props::container_act_request::detail {
namespace {

namespace caw = coop::props::container_act_wire;
namespace ci  = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace csw = coop::props::container_slice_wire;
namespace ct  = coop::props::container_target;
namespace cv  = ue_wrap::container_view;
namespace R   = ue_wrap::reflection;
namespace II  = ue_wrap::inventory_item;
namespace sg  = ue_wrap::script_gate;

// The seat: the inventory's addItemAdvanced, called by the screen for a drop on a canvas.
UE_FACT_FN(kRowAddAdvanced, "container_act", L"inventoryComponent_C", L"addItemAdvanced", Declared);
constexpr int kTagAddAdvanced = 0x43414334;   // "CAC4"

bool g_said = false;

// A read that tells a drop from any other call and fails leaves the call to run natively, unjudged: said once per
// session, so a layout change cannot turn the feature off silently.
sg::Verdict Unreadable(const char* field) {
    if (!g_said) {
        g_said = true;
        UE_LOGW("container_act: the drag put seat could not read %s -- the drop runs natively, unjudged", field);
    }
    return sg::Verdict::Run;
}

bool ReadHashes(void* inventory, std::vector<II::Item>& recs, std::vector<uint64_t>& hashes) {
    if (!inventory || !cio::ReadRecords(inventory, recs)) return false;
    hashes.clear();
    hashes.reserve(recs.size());
    for (const II::Item& r : recs) hashes.push_back(csw::RecordHash(r));
    return true;
}

// The screen's drop of an item widget's entry on the canvas of a shared inventory. Everything else that files an entry in
// an inventory (a world prop absorbed by a container, the vacuum, the loot a container spawns) is not a player's drop and
// runs natively, as does any inventory that is not shared.
sg::Verdict PreAddAdvanced(const sg::Call& c) {
    if (InReplay()) return sg::Verdict::Run;
    coop::net::Session* const s = LinkedSession();
    if (!s || s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    void* const inv = c.object;
    void* const screen = cv::Screen();
    if (!inv || !screen || c.callerObject != screen) return sg::Verdict::Run;
    uint32_t eid = 0;
    switch (SharedTargetOfInventory(inv, eid)) {
    case Target::Own:
        return sg::Verdict::Run;
    case Target::NotReady:
        ShowNotReady();
        return sg::Verdict::Cancel;
    case Target::Shared:
        break;
    }
    static R::ParamOffset sData{L"data"};
    static R::ParamOffset sSet{L"setPositionAndRotation"};
    static R::ParamOffset sPos{L"itemPosition"};
    static R::ParamOffset sRot{L"itemRotation"};
    const int32_t dataOff = sData.Of(c.function);
    const int32_t setOff = sSet.Of(c.function);
    const int32_t posOff = sPos.Of(c.function);
    const int32_t rotOff = sRot.Of(c.function);
    if (dataOff < 0 || setOff < 0 || posOff < 0 || rotOff < 0 || !c.locals) return Unreadable("addItemAdvanced's parameters");
    II::Item entry;
    if (!II::Read(c.locals + dataOff, entry)) return Unreadable("the entry being dropped");
    // The bag widget the entry comes from. A drop whose source cannot be named cannot be judged, and is never let through.
    void* const source = cv::SourceOf(entry);
    cv::WidgetEntry held;
    if (!source || !cv::ReadWidget(source, held) || (held.owner != inv && held.owner != cv::PlayerComponent())) {
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    // An entry dropped back on the canvas it lies on is a move within the inventory, which the host does not carry: the
    // widget stays where it was.
    if (held.owner == inv) return sg::Verdict::Cancel;
    if (c.locals[setOff] != 0) {
        float position[2] = {};
        float rotation = 0.f;
        std::memcpy(position, c.locals + posOff, sizeof(position));
        std::memcpy(&rotation, c.locals + rotOff, sizeof(rotation));
        entry.posX = position[0];
        entry.posY = position[1];
        entry.rotation = rotation;
    }
    return AskPutRecord(PutKind::Drag, eid, inv, entry, source);
}

void NotReplayed(const Pending& p, const char* why) {
    UE_LOGI("container_act: put id=%u eid=%u not replayed (%s) -> aborted", static_cast<unsigned>(p.id),
            static_cast<unsigned>(p.eid), why);
    SendCommit(p.id, p.eid, 0);
    ShowBadReason();   // the drop is lost, never the entry: it is still in the bag
}

}  // namespace

const WatchRow& DragWatch() {
    static const WatchRow kRow{kRowAddAdvanced, kTagAddAdvanced, &PreAddAdvanced, nullptr};
    return kRow;
}

// The granted drop: the inventory files the entry again under the latch if its widget still holds that item in the bag,
// then the widget leaves the bag, and the commit says where the inventory stored the entry. The inventory's records are
// read before and after; the commit is done exactly when one record was inserted, and carries it as the inventory stored it.
void ReplayDrag(const Pending& p) {
    void* const inv = p.container.Get();
    std::vector<II::Item> before;
    std::vector<uint64_t> beforeHashes;
    if (!ReadHashes(inv, before, beforeHashes)) {
        NotReplayed(p, "the inventory is gone");
        return;
    }
    void* const source = p.source.Get();
    cv::WidgetEntry held;
    if (!source || !cv::ReadWidget(source, held) || held.owner != cv::PlayerComponent()) {
        NotReplayed(p, "the item left the bag");
        return;
    }
    if (internal::PutHandHash(held.item) != p.hash || held.item.amount != p.entry.amount) {
        NotReplayed(p, "the item changed");
        return;
    }
    bool ran = false;
    {
        Latched latch;
        ran = ci::AddItemAdvanced(inv, p.entry);
    }
    // The replay changes the mirror under the latch, where no verb watch marks it, so the host's next truth for the target
    // must apply even when it equals the last one applied.
    ct::ForgetApplied(p.eid);
    std::vector<II::Item> after;
    std::vector<uint64_t> afterHashes;
    const bool read = ReadHashes(p.container.Get(), after, afterHashes);
    const std::optional<size_t> at = ran && read ? internal::StoredIndex(beforeHashes, afterHashes) : std::nullopt;
    if (at) {
        caw::Commit done;
        done.id = p.id;
        done.eid = p.eid;
        done.committed = 1;
        done.hasRecord = true;
        done.index = static_cast<uint16_t>(*at);
        done.record = after[*at];
        if (QueueCommit(done)) {   // false: the commit does not pack, nothing queued
            cv::Discard(source);   // the entry is in the inventory now: out of the bag
            UE_LOGI("container_act: put id=%u eid=%u replayed -> done at index %zu", static_cast<unsigned>(p.id),
                    static_cast<unsigned>(p.eid), *at);
            return;
        }
        // Cannot happen under the size gate; the inventory has the entry and the host will not.
        UE_LOGE("container_act: put id=%u eid=%u the stored %ls does not pack into a commit -- aborted",
                static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), II::ClassLeaf(done.record).c_str());
    }
    // The verb did not run, or the inventory stored nothing: the fit failed on this peer's mirror. The item never left
    // the bag.
    UE_LOGI("container_act: put id=%u eid=%u replayed ran=%d -> aborted (the inventory stored nothing)",
            static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), ran ? 1 : 0);
    SendCommit(p.id, p.eid, 0);
}

}  // namespace coop::props::container_act_request::detail
