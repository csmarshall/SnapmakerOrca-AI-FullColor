#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/UiDialogLog.hpp"

using namespace Slic3r::GUI;

TEST_CASE("ui dialog log line carries the stable tag, kind, title and text", "[ui_dialog_log]")
{
    CHECK(ui_dialog_log_line("message", "Send", "Upload failed") == "[ui-dialog] kind=message title=Send text=Upload failed");
    CHECK(ui_dialog_log_line("notification-error", "", "Error:\nprinter busy") == "[ui-dialog] kind=notification-error text=Error: | printer busy");
}

TEST_CASE("ui dialog log collapses newlines and control characters", "[ui_dialog_log]")
{
    CHECK(ui_dialog_log_clean("a\r\n\r\nb", 500) == "a | b");
    CHECK(ui_dialog_log_clean("a \n  b", 500) == "a | b");
    CHECK(ui_dialog_log_clean("\n\nlead and trail\n", 500) == "lead and trail");
    CHECK(ui_dialog_log_clean("tab\there\x01x", 500) == "tab here x");
    CHECK(ui_dialog_log_clean("", 500).empty());
}

TEST_CASE("ui dialog log truncates long text on a character boundary and says how much was cut", "[ui_dialog_log]")
{
    const std::string long_text(2000, 'x');
    const std::string out = ui_dialog_log_clean(long_text, kUiDialogLogMaxText);
    CHECK(out == std::string(500, 'x') + "...(+1500 bytes)");
    // 2-byte characters: a cut at an odd offset must not leave half a character
    std::string utf8;
    for (int i = 0; i < 400; ++i) utf8 += "\xC3\xA9";
    const std::string cut = ui_dialog_log_clean(utf8, 501);
    const std::string kept = cut.substr(0, cut.find("..."));
    CHECK(kept.size() % 2 == 0);
    CHECK(kept.size() == 500);
    CHECK(ui_dialog_log_clean(std::string(500, 'y'), 500) == std::string(500, 'y')); // exactly at the limit: untouched
}
