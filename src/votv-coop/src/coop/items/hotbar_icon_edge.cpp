// coop/items/hotbar_icon_edge.cpp -- see coop/items/hotbar_icon_edge.h.

#include "coop/items/hotbar_icon_edge.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/hotbar/icons.h"

#include <string>

namespace coop::hotbar_icon_edge {
namespace {

namespace HB = ue_wrap::hotbar;

// Asked a few times a second until it latches, then never again for that world. Each ask is a
// handful of pointer chases and TArray headers; the material walk below runs only once both icon
// tables are up, which is the last thing to happen in the window this watches.
constexpr int kPollTicks = 15;   // ~8 Hz at ~120 Hz

// Does the bar contradict its items -- a slot naming an item, icons published, and that slot still
// showing none? Each slot draws its own icon, so the first slot that names an item answers for the
// draws that ran before the icons were published.
//
// This asks about the OUTCOME rather than watching the tables go from empty to full, because the
// transition is not reliably observable: a world load runs as one long blocking call, and a run
// was measured in which the tables filled between the game's own refresh and this reader's first
// sample. A trigger armed on that edge would have missed it; a question about the bar is still
// true afterwards.
bool BuiltWithoutIcons(const HB::State& s) {
    if (!s.IconsReady() || s.firstFilled < 0) return false;
    const std::wstring tex = HB::SlotTexture(s, s.firstFilled);
    return tex == L"Black" || tex == L"null" || tex == L"no-parameter" || tex == L"no-material";
}

// Shape: MTA re-expresses what depends on an asset when the asset situation changes rather than
// assuming the dependants notice -- CClientGame::RestreamModel streams every entity of a model
// out so the streamer brings it back with the new art
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:6859). Deliberate divergence:
// MTA owns its streamer and can force the re-express directly, while the bar is the game's own
// widget, so the re-express here is the game's own draw verb called once per slot.
//
// The world this edge has finished with. Held as a CachedObjRef rather than a bare pointer: the
// engine reuses object slots, so a new world's renderer can land on the freed address of the old
// one and a pointer comparison would silently disarm the edge for that world.
ue_wrap::CachedObjRef g_settled;

}  // namespace

void Tick() {
    static int s_ticks = 0;
    if (++s_ticks < kPollTicks) return;
    s_ticks = 0;

    // The latch is tested first, on the renderer this world is running, and only then is the full
    // state read: an edge that fires once per world must cost nothing for the rest of it.
    HB::State s;
    if (!HB::Read(s) || !s.renderer) return;
    if (g_settled.Alive() && g_settled.Get() == s.renderer) return;

    if (!BuiltWithoutIcons(s)) {
        // Nothing to do, and nothing that can go wrong later: once the icons are published, every
        // rebuild the game runs from here on finds them. Settle, so a world whose player is
        // carrying nothing does not keep asking for the rest of its life.
        if (s.IconsReady()) {
            g_settled.Set(s.renderer);
            // Said on the settle as well as on the rebuild: a gate that only speaks when it acts
            // cannot report that it RAN, and this one spent a release unreachable on the launch
            // every player makes while its own readout was inside the branch that never ran.
            UE_LOGI("hotbar: the hotbar was drawn with its icons (names=%d texs=%d "
                    "filled=%d) -- nothing to redraw", s.iconNames, s.iconTextures, s.filled);
        }
        return;
    }

    g_settled.Set(s.renderer);
    const std::wstring before = HB::SlotTexture(s, s.firstFilled);
    const bool ok = HB::Rebuild(s);
    HB::State after;
    HB::Read(after);
    UE_LOGI("hotbar: the icon tables arrived after the hotbar was drawn (names=%d texs=%d phases=%d "
            "filled=%d) -- redrawn through each slot's upd: called=%d, slot%d %ls -> %ls",
            s.iconNames, s.iconTextures, s.rendererPhases, s.filled, ok ? 1 : 0, s.firstFilled, before.c_str(),
            HB::SlotTexture(after, s.firstFilled).c_str());
}

}  // namespace coop::hotbar_icon_edge
