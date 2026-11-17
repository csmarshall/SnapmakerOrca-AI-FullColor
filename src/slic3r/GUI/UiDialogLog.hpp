#ifndef slic3r_UiDialogLog_hpp_
#define slic3r_UiDialogLog_hpp_

// #138: one info-level log line per dialog, banner or notification the app shows, with the stable tag "[ui-dialog]", so a message that
// flashed by can be found in the log afterwards. Pure formatting (no wx, no logging) plus a one-line logger. Text only; never file contents.

#include <cstddef>
#include <string>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

// Longest text / title kept in the log line; the rest is replaced by a "...(+N bytes)" marker.
constexpr std::size_t kUiDialogLogMaxText  = 500;
constexpr std::size_t kUiDialogLogMaxTitle = 120;

// Newline runs (with the blanks around them) become " | ", other control characters become a space, then the result is cut to max_bytes
// without splitting a UTF-8 sequence. Nothing else is altered or redacted.
inline std::string ui_dialog_log_clean(const std::string& in, std::size_t max_bytes)
{
    std::string out;
    out.reserve(in.size());
    bool pending_break = false;
    for (unsigned char c : in) {
        if (c == '\n' || c == '\r') { pending_break = true; continue; }
        if (c == ' ' || c == '\t') { if (pending_break) continue; out += ' '; continue; }
        if (pending_break) {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (!out.empty()) out += " | ";
            pending_break = false;
        }
        out += (c < 0x20 || c == 0x7f) ? ' ' : char(c);
    }
    while (!out.empty() && (out.back() == ' ')) out.pop_back();
    if (out.size() > max_bytes) {
        std::size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut; // do not end inside a multi-byte character
        const std::size_t dropped = out.size() - cut;
        out.resize(cut);
        out += "...(+" + std::to_string(dropped) + " bytes)";
    }
    return out;
}

// "[ui-dialog] kind=<kind> title=<title> text=<text>" (title omitted when empty). kind is a short fixed word, e.g. message, rich, error,
// warning, info, notification-error, preprint-notify.
inline std::string ui_dialog_log_line(const std::string& kind, const std::string& title, const std::string& text)
{
    std::string line = "[ui-dialog] kind=" + kind;
    const std::string t = ui_dialog_log_clean(title, kUiDialogLogMaxTitle);
    if (!t.empty()) line += " title=" + t;
    line += " text=" + ui_dialog_log_clean(text, kUiDialogLogMaxText);
    return line;
}

inline void log_ui_dialog(const std::string& kind, const std::string& title, const std::string& text)
{
    BOOST_LOG_TRIVIAL(info) << ui_dialog_log_line(kind, title, text);
}

}} // namespace Slic3r::GUI

#endif
