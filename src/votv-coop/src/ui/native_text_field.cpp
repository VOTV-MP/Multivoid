// ui/native_text_field.cpp -- see ui/native_text_field.h for WHY this owns its own input.

#include "ui/native_text_field.h"

#include "ui/native_screen.h"

#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <vector>

namespace ui::native_text_field {

namespace NS = ui::native_screen;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace T  = coop::text;

struct Field {
    void*        box     = nullptr;   // the framed overlay; what we remove on destroy
    void*        parent  = nullptr;
    void*        text    = nullptr;   // the UTextBlock we drive
    std::wstring value;
    std::wstring hint;
    int32_t      maxLen  = 64;
    bool         focused = false;
    bool         submit  = false;     // Enter edge, consumed by the owner
    bool         dirty   = true;      // repaint owed
    bool         wasDown = false;     // left button edge, for click-to-focus
    uint64_t     caretAt = 0;         // tick count of the last caret phase flip
    bool         caretOn = false;
    std::string  utf8;                // cache so Text() can return a reference
};

namespace {

// EXACTLY ONE FIELD HOLDS THE KEYBOARD. A registry rather than a bare pointer because the
// detour runs on the game thread (`gate3`, 2026-07-31) but a screen can be torn down from
// its own tick in the same frame; the vector lets Destroy() clear the focus without the
// detour ever seeing a dangling handle.
std::vector<Field*> g_live;
std::atomic<Field*> g_focus{nullptr};

// Repaint budget: the caret blinks at the rate the game's own menus do, and a field that
// is not focused never blinks at all -- so an idle browser costs zero widget writes.
constexpr uint64_t kCaretMs = 530;

void Repaint(Field* f) {
    if (!f || !f->text) return;
    std::wstring shown;
    if (f->value.empty() && !f->focused) {
        shown = f->hint;
    } else {
        shown = f->value;
        if (f->focused && f->caretOn) shown += L'|';
    }
    E::SetWidgetText(f->text, shown.c_str());
    // The hint is dimmer than real content -- the style doc's secondary grey. A dimmed
    // placeholder is how a player tells "nothing typed yet" from "someone typed this".
    E::SetTextBlockColorDispatch(
        f->text, (f->value.empty() && !f->focused) ? NS::Dim() : NS::Text());
    f->dirty = false;
}

}  // namespace

Field* Create(void* parent, const wchar_t* hint, int32_t maxLen, float widthPx) {
    if (!parent) return nullptr;

    // A REAL SizeBox CARRIES THE WIDTH, and the frame goes INSIDE it.
    //
    // The first version called `SetSizeBoxWidth` on what `AddFramedBox` returns -- which
    // is an OVERLAY, as its own comment says. That writes a float at a SizeBox's property
    // offset into an object that has no such property, and the game paid for it three
    // frames later: `PE detour-outer-callback AV caught -- function='SpawnObject' ...
    // 0xC0000005` three times over, then the whole action bar failing to build
    // (`refresh=0 host=0 connect=0`) because the next spawns landed in corrupted memory.
    // The crash was NOT at the write; a wrong-offset write never is.
    void* sizer = NS::Spawn(L"SizeBox", parent);
    if (!sizer) return nullptr;
    if (widthPx > 0.f) U::SetSizeBoxWidth(sizer, widthPx);

    void* box = NS::AddFramedBox(sizer, NS::RowBg(), 2.f);
    if (!box) return nullptr;
    // The frame is the hit target, so it must be a real Visible widget rather than the
    // HitTestInvisible chrome AddFramedBox gives its two images.
    E::SetWidgetVisibility(box, 0);
    void* tb = NS::AddText(box, hint ? hint : L"", NS::kBtnFontPx, NS::Dim(), NS::kLeft, 0.f);
    if (!tb) return nullptr;
    // CLIP TO THE BOX. `AddText` only sets clipping for a FILL slot, and this one is
    // auto-sized, so a value longer than the field painted straight out of its own frame
    // and across whatever sat beside it. The user saw exactly that on the first build that
    // shipped a field: "ебаный текст в ебаное окно ввода ip не помещается".
    // EWidgetClipping::ClipToBounds = 1.
    U::SetClipping(tb, 1);

    // ATTACH, and attach at BIRTH. `AddFramedBox` spawns with `parent` as the OUTER but
    // does not add the widget to it -- every other caller in this tree does its own
    // AddChild, and the first version of this function did not. An unattached widget tree
    // is GC food: it renders until the next collection and then vanishes, which this
    // project has already paid for once with the MULTIPLAYER button
    // (`[[lesson-an-unattached-widget-tree-is-gc-food]]`).
    U::SetContent(sizer, box);
    if (!U::AddChild(parent, sizer)) return nullptr;

    auto* f = new Field();
    f->box    = sizer;   // the OUTERMOST widget -- what attaches, and what must be removed
    f->parent = parent;
    f->text   = tb;
    f->hint   = hint ? hint : L"";
    f->maxLen = maxLen > 0 ? maxLen : 64;
    g_live.push_back(f);
    Repaint(f);
    return f;
}

void Destroy(Field* f) {
    if (!f) return;
    // Clear focus FIRST: the detour reads g_focus without a lock, so the window in which
    // it could reach a half-destroyed field has to be closed before anything is freed.
    Field* expect = f;
    g_focus.compare_exchange_strong(expect, nullptr);
    for (size_t i = 0; i < g_live.size(); ++i)
        if (g_live[i] == f) { g_live.erase(g_live.begin() + static_cast<long>(i)); break; }
    if (f->box && f->parent) U::RemoveChild(f->parent, f->box);
    delete f;
}

void Focus(Field* f) {
    if (!f) return;
    Field* prev = g_focus.exchange(f);
    if (prev && prev != f) { prev->focused = false; prev->dirty = true; Repaint(prev); }
    f->focused = true;
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    Repaint(f);
}

void Blur(Field* f) {
    if (!f) return;
    Field* expect = f;
    g_focus.compare_exchange_strong(expect, nullptr);
    f->focused = false;
    f->caretOn = false;
    f->dirty   = true;
    Repaint(f);
}

bool Focused(const Field* f) { return f && f->focused; }
bool AnyFocused()            { return g_focus.load() != nullptr; }

const std::string& Text(const Field* f) {
    static const std::string kEmpty;
    if (!f) return kEmpty;
    auto* m = const_cast<Field*>(f);
    m->utf8 = T::ToUtf8(f->value);
    return m->utf8;
}

void SetText(Field* f, const std::string& utf8) {
    if (!f) return;
    // Lossy on the way IN is right: this is a local string the player or our own ini
    // supplied, and refusing it whole would leave the field mysteriously empty. The
    // STRICT boundary is the wire, and it is elsewhere.
    f->value = T::CapCodepoints(T::FromUtf8Lossy(utf8.data(), utf8.size()),
                                static_cast<size_t>(f->maxLen));
    f->dirty = true;
    Repaint(f);
}

void Tick(Field* f) {
    if (!f) return;

    // CLICK TO FOCUS, by geometry -- the same mechanism the rows and the NEW GAME row use,
    // and the only one measured to work in this tree. A press anywhere else blurs, so a
    // player clicking a server row is not still typing into the address box.
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !f->wasDown) {
        if (NS::CursorOverWidget(f->box)) Focus(f);
        else if (f->focused)              Blur(f);
    }
    f->wasDown = down;

    if (f->focused) {
        const uint64_t now = ::GetTickCount64();
        if (now - f->caretAt >= kCaretMs) {
            f->caretAt = now;
            f->caretOn = !f->caretOn;
            f->dirty   = true;
        }
    }
    if (f->dirty) Repaint(f);
}

bool ConsumeSubmit(Field* f) {
    if (!f || !f->submit) return false;
    f->submit = false;
    return true;
}

bool OnChar(wchar_t c) {
    Field* f = g_focus.load();
    if (!f) return false;
    // Control characters are not content. Backspace and Enter arrive as WM_CHAR too on
    // some layouts, and they are handled on the KEYDOWN edge instead -- taking them here
    // as well would type a box glyph AND act on them.
    if (c < 0x20 || c == 0x7F) return true;   // swallowed, deliberately not inserted
    if (static_cast<int32_t>(T::CountCodepoints(f->value)) >= f->maxLen) return true;
    f->value += c;
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    Repaint(f);
    return true;
}

// ---- SELFTEST -----------------------------------------------------------------------
//
// UN-GATED, and it runs on every boot. The editing rules here are PURE -- append, cap,
// backspace, the Enter edge, Escape -- so they need no widget and no game to exercise:
// a Field with null `box`/`text` makes Repaint a no-op and everything else is the real
// production path, `OnChar` and `OnKeyDown` included. This is deliberately not a dev flag.
// Three separate instruments today reported confident wrong answers because nobody had
// run them against the failing axis; a test that only runs when someone remembers to arm
// it is the same bet.
//
// What it CANNOT cover, stated so the coverage is not overclaimed: whether the WndProc
// seam actually delivers a keystroke to this module. That is a live-process question and
// belongs to a run, not to a unit check.
bool RunSelftest() {
    int checks = 0, failed = 0;
    auto ok = [&](bool cond, const char* what) {
        ++checks;
        if (!cond) { ++failed; UE_LOGE("native_text_field selftest FAIL: %s", what); }
    };

    // STATIC, not a stack local, and the reason is a real hazard rather than style:
    // `Focus()` publishes this pointer into `g_focus`, which the WndProc detour reads from
    // the GAME thread. A stack object would leave that global pointing at a dead frame the
    // moment this function returned -- the window is microseconds and the detour is not
    // even installed this early in boot, so it has probably never fired, but "probably
    // never" is not a lifetime argument. A function-local static cannot dangle, and the
    // final check below still proves the focus was released.
    static Field f;               // no widgets: Repaint no-ops, the logic is untouched
    f.maxLen = 5;
    Focus(&f);
    ok(AnyFocused(), "focus is taken");

    ok(OnChar(L'1') && OnChar(L'2'), "printable characters are consumed");
    ok(Text(&f) == "12", "typed characters land in the value");

    ok(OnChar(L'\r'), "a control character is consumed");
    ok(Text(&f) == "12", "...but is NOT inserted as content");

    ok(OnKeyDown(VK_BACK), "backspace is consumed");
    ok(Text(&f) == "1", "backspace removes one character");

    OnChar(L'2'); OnChar(L'3'); OnChar(L'4'); OnChar(L'5');
    ok(Text(&f) == "12345", "the field fills to maxLen");
    OnChar(L'6');
    ok(Text(&f) == "12345", "maxLen is a CAP, not a suggestion");

    ok(!ConsumeSubmit(&f), "no submit before Enter");
    ok(OnKeyDown(VK_RETURN), "Enter is consumed");
    ok(ConsumeSubmit(&f), "Enter raises the submit edge");
    ok(!ConsumeSubmit(&f), "...and the edge is consumed ONCE");

    // A surrogate pair is ONE character to the player. Deleting half of it would leave an
    // unpaired surrogate in a string that is about to be encoded to UTF-8.
    f.maxLen = 8;
    SetText(&f, "");
    OnChar(static_cast<wchar_t>(0xD83D)); OnChar(static_cast<wchar_t>(0xDE00));  // U+1F600
    const size_t pairLen = Text(&f).size();
    ok(pairLen == 4, "an astral character encodes to 4 UTF-8 bytes");
    OnKeyDown(VK_BACK);
    ok(Text(&f).empty(), "backspace removes the WHOLE surrogate pair");

    ok(OnKeyDown(VK_ESCAPE), "Escape is consumed");
    ok(!AnyFocused(), "Escape leaves the field");
    ok(!OnChar(L'x'), "an unfocused module refuses the key so the game still gets it");

    // Blur() cleared g_focus; the local Field is about to die, so nothing may still point
    // at it. (Destroy() is not used here: it would try to touch a parent that never existed.)
    ok(g_focus.load() == nullptr, "no dangling focus at teardown");

    if (failed == 0) UE_LOGI("native_text_field selftest: ALL PASS (%d checks)", checks);
    else             UE_LOGE("native_text_field selftest: %d/%d FAILED", failed, checks);
    return failed == 0;
}

bool OnKeyDown(int vk) {
    Field* f = g_focus.load();
    if (!f) return false;
    switch (vk) {
        case VK_BACK:
            if (!f->value.empty()) {
                // Pop a whole CODEPOINT: a surrogate pair is one character to the player,
                // and deleting half of it leaves an unpaired surrogate in a string that is
                // about to be encoded to UTF-8.
                size_t n = f->value.size();
                if (n >= 2 && (f->value[n - 1] & 0xFC00) == 0xDC00 &&
                              (f->value[n - 2] & 0xFC00) == 0xD800) f->value.resize(n - 2);
                else                                                f->value.resize(n - 1);
                f->dirty = true;
                Repaint(f);
            }
            return true;
        case VK_RETURN:
            f->submit = true;
            return true;
        case VK_ESCAPE:
            // Escape LEAVES THE FIELD, it does not close the screen. Swallowing the
            // message is NOT enough to make that true: the browser reads Escape with
            // GetAsyncKeyState, which sees the physical key whatever the detour does, so
            // the screen defers to `AnyFocused()` on its own poll edge. Both halves are
            // required and neither works alone.
            Blur(f);
            return true;
        default:
            // Everything else falls through to whoever else wants it. A focused address
            // box has no business swallowing F1 or the movement keys.
            return false;
    }
}

}  // namespace ui::native_text_field
