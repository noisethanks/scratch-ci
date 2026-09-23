#pragma once

#include <exception>
#include <string>
#include <string_view>

#include <plugins/PluginAPI.hpp>

// Failure reporting for the plugin. debug:disable_logs defaults to true, so
// Hyprland's log alone is invisible to users: every report is also shown as a
// notification and written in full to a per-session error file.
//
// Contract, for current and future callers (config values, shader/file
// loading):
//  - report() is safe from any context, including inside a render: the
//    notification is deferred to the event loop via doLater.
//  - Hyprland's log gets every report. The notification and the error file
//    get each key once per plugin load; resetKey() re-arms a key (e.g. after
//    a config reload or once a missing file shows up).
//  - Keys name the failing thing, not the message: "shader:trail",
//    "gl:trail", "hook", "callback:<name>", later "config:<value>",
//    "file:<path>".
//  - ERR = a feature is off; WARN = degraded but running.
//  - Nothing here throws.
namespace hyprtail::diag {
    enum class eSeverity {
        WARN,
        ERR,
    };

    // Call first thing in PLUGIN_INIT. Truncates the error file.
    void init(HANDLE handle) noexcept;

    // Call last thing in PLUGIN_EXIT. Cancels any pending deferred
    // notification, which would otherwise run code from the unloaded .so.
    void        shutdown() noexcept;

    void        report(eSeverity severity, std::string_view key, std::string_view message) noexcept;
    void        resetKey(std::string_view key) noexcept;

    // Path of the per-session error file, empty if it couldn't be opened.
    std::string errorFilePath() noexcept;

    void        reportException(std::string_view where, std::exception_ptr ex) noexcept;

    // Runs fn, reports anything it throws under "callback:<where>" and returns
    // false instead of letting it escape (into Hyprland, the event loop, ...).
    template <typename F>
    bool guard(std::string_view where, F&& fn) noexcept {
        try {
            fn();
            return true;
        } catch (...) { reportException(where, std::current_exception()); }
        return false;
    }
}
