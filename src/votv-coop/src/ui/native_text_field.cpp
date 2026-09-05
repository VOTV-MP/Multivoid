// ui/native_text_field.cpp -- see ui/native_text_field.h for why this owns its own input.

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
    // The Escape edge, consumed by the owner: the screens take their edge on key-up, by which
    // time the blur (on key-down) has already happened, so a focus query cannot answer; this latch
    // can.
    bool         ateEscape = false;
    bool         dirty   = true;      // repaint owed
    bool         wasDown = false;     // left button edge, for click-to-focus
    uint64_t     caretAt = 0;         // tick count of the last caret phase flip
    bool         caretOn = false;
    std::string  utf8;                // cache so Text() can return a reference
    // The overflow window (see UpdateWindowing).
    bool         alignRight  = false;   // the slot alignment currently in force
    size_t       measuredLen = static_cast<size_t>(-1);  // value length when last measured
    int          remeasureIn = -1;      // ticks until the pending measurement; -1 = none
};

namespace {

// Exactly one field holds the keyboard. A registry rather than a bare pointer: the detour runs
// on the game thread, but a screen can be torn down from its own tick in the same frame, and
// the vector lets Destroy clear the focus without the detour seeing a dangling handle.
std::vector<Field*> g_live;
std::atomic<Field*> g_focus{nullptr};

// The repaint budget: the caret blinks at the rate the game's menus do, and an unfocused field
// never blinks, so an idle browser costs no widget writes.
constexpr uint64_t kCaretMs = 530;

// Forward-declared: the windowing and paste helpers repaint, and they are grouped by concept.
void Repaint(Field* f);

// The frame Create puts around the text and the gutter its glyphs sit in; UpdateWindowing
// subtracts them to know the room the text has.
constexpr float kFrameBorderPx = 2.f;
constexpr float kTextGutterPx  = 8.f;

// Which end of an over-long value the player sees, and it must be the end they type at. The
// block is clipped to its box, and a left-aligned clipped block keeps its head, so past the
// box's width the player would type into a part of the string that is not drawn, caret
// included. The fix is an alignment flip, not a trim: Slate clips whichever end the alignment
// pushes out, so a right-aligned block in a clipped frame shows the tail and the caret. One
// enum, any font and size, no glyph measuring, and the value is never touched. Measured one
// tick late on purpose: the desired size reads the built widget, so a value set this frame
// would measure at its previous width; and keyed on the value's length rather than run per
// repaint, since the caret changes the drawn width by one glyph twice a second.
void UpdateWindowing(Field* f) {
    if (!f || !f->text || !f->box) return;
    if (f->remeasureIn < 0) return;
    if (f->remeasureIn-- > 0) return;   // the deferred tick has not arrived yet
    ue_wrap::FVector2D desired{}, topLeft{}, allotted{};
    // The guards run before the measured length is claimed: a measurement that never happened
    // must not disarm the retry, since MarkValueChanged re-arms only on a length change, and a
    // screen shown with an over-long prefill (not yet arranged, so a zero rect) would keep left
    // alignment until the player typed.
    auto retry = [f] { f->remeasureIn = 1; };
    if (!U::WidgetDesiredSize(f->text, desired)) { retry(); return; }
    if (!U::WidgetScreenRect(f->box, topLeft, allotted)) { retry(); return; }
    const float inner = allotted.X - 2.f * kFrameBorderPx - kTextGutterPx;
    // A widget Slate has never laid out reports a zero rect, a real answer but not one to act on;
    // ask again next tick.
    if (inner <= 0.f || desired.X <= 0.f) { retry(); return; }
    f->measuredLen = f->value.size();
    const bool want = desired.X > inner;
    if (want == f->alignRight) return;
    f->alignRight = want;
    U::SetSlotHAlignLive(NS::SlotOf(f->text), want ? NS::kRight : NS::kLeft);
}

// The value changed: schedule the measurement for the tick after Slate has laid it out.
void MarkValueChanged(Field* f) {
    if (f && f->value.size() != f->measuredLen) f->remeasureIn = 1;
}

// Paste, here rather than in each screen: an address or a nickname is exactly what a player
// has in the clipboard, and a field that accepts typing but not pasting reads as broken. One
// implementation, so every field pastes the same way. Trimmed at entry, since a copied address
// usually carries a trailing newline or a leading space (MTA trims its connect string at the
// same point). Appended, not replacing: the grammar is append, backspace and paste with no
// selection, so paste behaves like typing, with the same cap, sanitising and windowing. The
// pure half, split from the clipboard read so the selftest can drive it without writing to the
// player's clipboard.
void ApplyPastedText(Field* f, const std::wstring& in) {
    if (!f || in.empty()) return;
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' ||
                                          c == L'\n' || c == L'\f' || c == L'\v'; };
    size_t b = 0, e = in.size();
    while (b < e && isSpace(in[b])) ++b;
    while (e > b && isSpace(in[e - 1])) --e;
    // Control characters are not content, the rule OnChar applies per keystroke; an embedded
    // newline would arrive as a glyph nothing can type.
    std::wstring add;
    for (size_t i = b; i < e; ++i)
        if (in[i] >= 0x20 && in[i] != 0x7F) add += in[i];
    if (add.empty()) return;

    f->value = T::CapCodepoints(f->value + add, static_cast<size_t>(f->maxLen));
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    MarkValueChanged(f);
    Repaint(f);
}

void Paste(Field* f) {
    if (!f) return;
    if (!::OpenClipboard(nullptr)) return;
    std::wstring in;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
            // Bounded by the field's cap plus slack for what the trim removes; a clipboard can hold
            // megabytes.
            const size_t cap = static_cast<size_t>(f->maxLen) * 4 + 64;
            for (size_t i = 0; i < cap && p[i]; ++i) in += p[i];
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    ApplyPastedText(f, in);
}

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
    // The hint is dimmer than content, the secondary grey: a dimmed placeholder is how a player
    // tells nothing-typed from typed.
    E::SetTextBlockColorDispatch(
        f->text, (f->value.empty() && !f->focused) ? NS::Dim() : NS::Text());
    f->dirty = false;
}

}  // namespace

Field* Create(void* parent, const wchar_t* hint, int32_t maxLen, float widthPx) {
    if (!parent) return nullptr;

    // A real SizeBox carries the width, and the frame goes inside it. Writing a size-box width
    // onto the overlay AddFramedBox returns lands a float at a property offset the object does
    // not have, and the fault surfaces frames later in unrelated spawns, never at the write.
    void* sizer = NS::Spawn(L"SizeBox", parent);
    if (!sizer) return nullptr;
    if (widthPx > 0.f) U::SetSizeBoxWidth(sizer, widthPx);

    void* box = NS::AddFramedBox(sizer, NS::RowBg(), 2.f);
    if (!box) return nullptr;
    // The frame is the hit target, so it must be a real visible widget rather than the
    // hit-test-invisible chrome AddFramedBox gives its images.
    E::SetWidgetVisibility(box, 0);
    void* tb = NS::AddText(box, hint ? hint : L"", NS::kBtnFontPx, NS::Dim(), NS::kLeft, 0.f);
    if (!tb) return nullptr;
    // Clip to the box: AddText only sets clipping for a fill slot, and this one is auto-sized, so
    // a value longer than the field would paint out of its frame across whatever sits beside it.
    // ClipToBounds is 1.
    U::SetClipping(tb, 1);

    // Attach, and attach at birth: AddFramedBox spawns with `parent` as the outer but does not add
    // the widget to it, and an unattached widget tree is GC food, rendering until the next
    // collection and then vanishing.
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

// Everything teardown does except touching the engine: a caller running on a dead menu
// instance must not dispatch into its widgets.
void Release(Field* f) {
    if (!f) return;
    Field* expect = f;
    g_focus.compare_exchange_strong(expect, nullptr);
    for (size_t i = 0; i < g_live.size(); ++i)
        if (g_live[i] == f) { g_live.erase(g_live.begin() + static_cast<long>(i)); break; }
    delete f;
}

void Destroy(Field* f) {
    if (!f) return;
    // Focus is cleared first: the detour reads the focus without a lock, so the window in which it
    // could reach a half-destroyed field must close before anything is freed.
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
    // Lossy on the way in is right: a local string from the player or our ini, and refusing it
    // whole would leave the field mysteriously empty. The strict boundary is the wire.
    f->value = T::CapCodepoints(T::FromUtf8Lossy(utf8.data(), utf8.size()),
                                static_cast<size_t>(f->maxLen));
    f->dirty = true;
    MarkValueChanged(f);
    Repaint(f);
}

void Tick(Field* f) {
    if (!f) return;

    // Click to focus, by geometry, the mechanism the rows use; a press anywhere else blurs, so a
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
    // After the repaint: the measurement is about the text just written, and it is deferred a tick
    // internally anyway.
    UpdateWindowing(f);
}

bool ConsumeEscape(Field* f) {
    if (!f || !f->ateEscape) return false;
    f->ateEscape = false;
    return true;
}

bool ConsumeSubmit(Field* f) {
    if (!f || !f->submit) return false;
    f->submit = false;
    return true;
}

bool OnChar(wchar_t c) {
    Field* f = g_focus.load();
    if (!f) return false;
    // Control characters are not content. Backspace and Enter arrive as WM_CHAR too on some
    // layouts and are handled on the key-down edge; taking them here as well would type a box
    // glyph and act on them.
    if (c < 0x20 || c == 0x7F) return true;   // swallowed, deliberately not inserted
    if (static_cast<int32_t>(T::CountCodepoints(f->value)) >= f->maxLen) return true;
    f->value += c;
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    MarkValueChanged(f);
    Repaint(f);
    return true;
}

// The selftest, un-gated, on every boot. The editing rules are pure (append, cap, backspace,
// the Enter edge, Escape), so a Field with no widgets makes Repaint a no-op and everything
// else is the production path, OnChar and OnKeyDown included; a test that runs only when
// someone remembers to arm it is no test. It cannot cover whether the WndProc seam delivers a
// keystroke to this module; that belongs to a run.
bool RunSelftest() {
    int checks = 0, failed = 0;
    auto ok = [&](bool cond, const char* what) {
        ++checks;
        if (!cond) { ++failed; UE_LOGE("native_text_field selftest FAIL: %s", what); }
    };

    // Static, not a stack local: Focus publishes this pointer into the focus global, which the
    // WndProc detour reads from the game thread, and a stack object would leave that global
    // pointing at a dead frame. The final check still proves the focus was released.
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

    // A surrogate pair is one character to the player; deleting half would leave an unpaired
    // surrogate in a string about to be encoded.
    f.maxLen = 8;
    SetText(&f, "");
    OnChar(static_cast<wchar_t>(0xD83D)); OnChar(static_cast<wchar_t>(0xDE00));  // U+1F600
    const size_t pairLen = Text(&f).size();
    ok(pairLen == 4, "an astral character encodes to 4 UTF-8 bytes");
    OnKeyDown(VK_BACK);
    ok(Text(&f).empty(), "backspace removes the WHOLE surrogate pair");

    // Paste through the pure half; the clipboard read is one call above it.
    f.maxLen = 24;
    SetText(&f, "");
    ApplyPastedText(&f, L"  10.0.0.5:7777\r\n");
    ok(Text(&f) == "10.0.0.5:7777", "paste trims leading and trailing whitespace");
    SetText(&f, "");
    ApplyPastedText(&f, L"a\nb\tc");
    ok(Text(&f) == "abc", "paste drops embedded control characters");
    SetText(&f, "abc");
    ApplyPastedText(&f, L"def");
    ok(Text(&f) == "abcdef", "paste APPENDS -- the grammar has no selection to replace");
    f.maxLen = 4;
    SetText(&f, "ab");
    ApplyPastedText(&f, L"cdefgh");
    ok(Text(&f) == "abcd", "paste obeys maxLen");
    SetText(&f, "keep");
    ApplyPastedText(&f, L"   \r\n  ");
    ok(Text(&f) == "keep", "an all-whitespace paste changes nothing");
    f.maxLen = 8;

    ok(!ConsumeEscape(&f), "no escape edge before Escape");
    ok(OnKeyDown(VK_ESCAPE), "Escape is consumed");
    ok(!AnyFocused(), "Escape leaves the field");
    // The latch survives the blur, which is the point: the owner asks after the field has let go.
    ok(ConsumeEscape(&f), "the escape edge is readable after the blur");
    ok(!ConsumeEscape(&f), "and it is consumed exactly once");
    ok(!OnChar(L'x'), "an unfocused module refuses the key so the game still gets it");

    // Blur cleared the focus; nothing may still point at the local Field. Destroy is not used,
    // since it would touch a parent that never existed.
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
                // Pop a whole codepoint: a surrogate pair is one character to the player.
                size_t n = f->value.size();
                if (n >= 2 && (f->value[n - 1] & 0xFC00) == 0xDC00 &&
                              (f->value[n - 2] & 0xFC00) == 0xD800) f->value.resize(n - 2);
                else                                                f->value.resize(n - 1);
                f->dirty = true;
                MarkValueChanged(f);
                Repaint(f);
            }
            return true;
        case 'V':
            // Ctrl+V. The modifier is read here because the detour hands over only the virtual key,
            // and it runs synchronously on the message, so GetKeyState reports the state the key
            // was pressed with. A bare V is content and falls through to WM_CHAR.
            if ((::GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;
            Paste(f);
            return true;
        case VK_RETURN:
            f->submit = true;
            return true;
        case VK_ESCAPE:
            // Escape leaves the field; it does not close the screen. Swallowing the message is not
            // enough, since the screens read Escape from the physical key state; and a focus query
            // is the wrong question for them, because this runs on key-down, before the key-up they
            // take their edge on, so the field has already blurred by the time they ask. The latch
            // says an Escape was consumed here, and cannot race, since the same event sets it.
            f->ateEscape = true;
            Blur(f);
            return true;
        default:
            // Everything else falls through: a focused address box has no business swallowing F1 or
            // the movement keys.
            return false;
    }
}

}  // namespace ui::native_text_field
