// coop/text/exclude_ranges.inc -- GENERATED from the fonts on disk, never hand-edited. Re-mint
// when a face or the donor changes, or this and the atlas fall out of step with nothing to say so.
//
// THE EXCLUDE SET, handed to ImGui as ImFontConfig::GlyphExcludeRanges on every font source. It
// is a COARSE cover, not a complement: it holds every codepoint the render set carries and
// repertoire_ranges.inc omits -- save U+0000, below -- and nothing that table holds. 143797
// codepoints in 32 ranges, only 1884 of them in the render set at all. ImGui asserts 64 values
// or fewer at imgui_draw.cpp:3115 because it scans the list per codepoint; that is advisory (a
// longer scan, not a break) and NDEBUG strips it. Both tables come from ONE source set in one
// run, which keeps `fold == bake` true under a lazy atlas.
//
// U+0000 IS DELIBERATELY ABSENT and must stay absent: ImGui walks this list as a zero-terminated
// array, so a leading zero would silently make the whole list a no-op. It is the one codepoint
// the two emissions differ by, asserted by the generator; repertoire.cpp's static_assert covers
// only the narrower half, that the table does not begin at zero. ImGui text is NUL-terminated.

    { 0x00001, 0x0001F },
    { 0x0007F, 0x000A0 },
    { 0x000AD, 0x000AD },
    { 0x0034F, 0x0034F },
    { 0x00600, 0x00605 },
    { 0x0061C, 0x0061C },
    { 0x006DD, 0x006DD },
    { 0x0070F, 0x0070F },
    { 0x00890, 0x00891 },
    { 0x008E2, 0x008E2 },
    { 0x0115F, 0x01160 },
    { 0x01680, 0x01680 },
    { 0x017B4, 0x017B5 },
    { 0x0180B, 0x0180F },
    { 0x02000, 0x0200F },
    { 0x02028, 0x0202F },
    { 0x0205F, 0x0206F },
    { 0x03000, 0x03000 },
    { 0x03164, 0x03164 },
    { 0x0D800, 0x0F8FF },
    { 0x0FE00, 0x0FE0F },
    { 0x0FEFF, 0x0FEFF },
    { 0x0FFA0, 0x0FFA0 },
    { 0x0FFF0, 0x0FFFB },
    { 0x110BD, 0x110BD },
    { 0x110CD, 0x110CD },
    { 0x13430, 0x13438 },
    { 0x1BCA0, 0x1BCA3 },
    { 0x1D173, 0x1D17A },
    { 0xE0000, 0xE0FFF },
    { 0xF0000, 0xFFFFD },
    { 0x100000, 0x10FFFD },
