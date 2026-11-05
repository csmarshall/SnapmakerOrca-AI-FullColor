#ifndef slic3r_GUI_PostProcessor_hpp_
#define slic3r_GUI_PostProcessor_hpp_

#include <functional>
#include <string>

namespace Slic3r { class DynamicPrintConfig; class AppConfig; }


namespace Slic3r {

// Run post-processing scripts (the "post_process" option) and/or the slicing-pipeline plugins'
// Step.psGCodePostProcess seam (the "slicing_pipeline_plugin" option) if defined. Lives in the GUI
// layer because plugins are executed through the embedded-Python PluginManager, which libslic3r must
// not depend on.
// Returns true if a script or plugin was executed.
// Returns false if neither a post-processing script nor plugin was defined.
// A failure of the scripts or plugins, or of the file work around them, throws Slic3r::ExportError: it fails
// this export or upload, not the slice. A cancel, a fatal error and running out of memory keep their own
// exceptions.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// If make_copy, then a temp file will be created for src_path by adding a ".pp" suffix and src_path will be updated.
// In that case the caller is responsible to delete the temp file created. Scripts and plugins always
// run on this working copy so they never touch the original G-code the viewer keeps memory-mapped
// (a writable open of the mapped file fails on Windows with a sharing violation).
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
extern bool run_post_process_scripts(
    std::string& src_path, bool make_copy, const std::string& host, std::string& output_name, const DynamicPrintConfig& config);

inline bool run_post_process_scripts(std::string& src_path, const DynamicPrintConfig& config)
{
    std::string src_path_name = src_path;
    return run_post_process_scripts(src_path, false, "File", src_path_name, config);
}

// Approval of the post_process value (programs that run on the host after export).
//
// post_process can be set by a preset file written by anything (an evasive plugin, an editor, a
// bundle), so before scripts run the SHA-256 of the exact value must be in the set of values the user
// approved in the GUI. The set lives in AppConfig section "approved_post_process"; nothing but the GUI
// prompt below adds to it (plugins cannot write OrcaSlicer.conf, the Remote API has no way to).
//
// Hex SHA-256 of the post_process value of `config`, or an empty string when it holds no script.
extern std::string post_process_hash(const DynamicPrintConfig& config);
extern bool        is_post_process_hash_approved(const AppConfig& app_config, const std::string& hash);
extern void        approve_post_process_hash(AppConfig& app_config, const std::string& hash);

enum class PostProcessApproval { NotNeeded, Approved, NeedsApproval };
// Reads the application's AppConfig. Safe from any thread; never shows UI.
extern PostProcessApproval check_post_process_approval(const DynamicPrintConfig& config);
// Asks the user (modal, UI thread only) to approve the scripts in `config`, and records a Yes. Returns
// true when the value is approved afterwards. Never call it from the Remote API or a worker thread.
extern bool ask_post_process_approval(const DynamicPrintConfig& config);
// Error text of the ExportError thrown when scripts are about to run unapproved on a thread that cannot ask.
inline constexpr const char* post_process_needs_approval = "post_process_needs_approval";

// BBS
extern void gcode_add_line_number(const std::string& path, const DynamicPrintConfig& config);
} // namespace Slic3r

#endif /* slic3r_GUI_PostProcessor_hpp_ */
