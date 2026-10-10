// coop/bug_report/report_form.cpp -- see coop/bug_report/report_core.h.

#include "coop/bug_report/report_core.h"

#include "l10n/mark.h"
#include "ue_wrap/core/log.h"

namespace coop::bug_report {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Code points of a UTF-8 string: every byte that is not a continuation byte (10xxxxxx).
size_t CodePoints(std::string_view s) {
    size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

std::string_view Trimmed(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

const char* ValidateForm(const Form& f) {
    if (CodePoints(Trimmed(f.happened)) < kHappenedMinChars)
        return L10N_MARK("Please describe what happened (at least 20 characters).");
    if (f.happened.size() > kFieldMaxBytes || f.expected.size() > kFieldMaxBytes ||
        f.contact.size() > kContactMaxBytes)
        return L10N_MARK("That text is too long.");
    return nullptr;
}

std::string ReportText(const Form& f) {
    return "What happened:\n" + f.happened + "\n\nWhat you expected:\n" + f.expected +
           "\n\nContact: " + f.contact + "\n";
}

bool IsReadableLogFormat(std::string_view line2) {
    if (!line2.empty() && line2.back() == '\r') line2.remove_suffix(1);
    return line2 == ue_wrap::log::kLogFormatLine;
}

}  // namespace coop::bug_report
