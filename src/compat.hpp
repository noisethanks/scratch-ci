#pragma once

// The one place hyprtail adapts to differences between Hyprland checkouts
// (the pin, SPEC §2, and main). Each divergence is detected by what the
// headers offer, never by a commit or version number, and everything else
// calls the wrapper here instead of the Hyprland API directly.
//
// Detection is a `requires` expression on a template parameter wherever the
// name exists in both trees. Two things can't be: a header that moved and a
// type that was replaced under a new name. Naming an undeclared type is a hard
// error outside a template, so those use __has_include.
//
// A discarded `if constexpr` branch is only skipped when it depends on a
// template parameter, so each wrapper takes the Hyprland type as one.
//
// Divergences handled (pin efb5099 vs main 4bb6844, NOTES "Compatibility"):
//   - Log::CLogger::log() takes a location string on main (Logger.hpp:41,45).
//     Its 3-argument form with runtime strings still compiles when called the
//     old way and then prints the format string as the location, so nothing
//     outside this file may call Log::logger->log() (`make check-log`).
//   - CHyprOpenGLImpl::bindArrayBuffer() caches the binding on main
//     (OpenGL.hpp:229); a raw glBindBuffer desyncs it.
//   - CWindow::m_class / m_title became metadata().appID() / title().
//   - Window.hpp moved to desktop/view/window/.
//   - hyprctl commands: SHyprCtlCommand became IPC::Socket1::SCommand.

#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <plugins/PluginAPI.hpp>
#include <debug/log/Logger.hpp>
#include <render/OpenGL.hpp>

#if __has_include(<desktop/view/window/Window.hpp>)
#include <desktop/view/window/Window.hpp>
#include <desktop/view/window/WindowMetadata.hpp>
#else
#include <desktop/view/Window.hpp>
#endif

#if __has_include(<ipc/s1/S1.hpp>)
#include <ipc/s1/S1.hpp>
#define HYPRTAIL_COMPAT_IPC_S1 1
#endif

namespace hyprtail::compat {

    // ---------------------------------------------------------------- logging

    namespace detail {
        template <typename L>
        inline constexpr bool logTakesLoc = requires { static_cast<void (L::*)(Hyprutils::CLI::eLogLevel, std::string_view, std::string_view)>(&L::log); };

        template <typename L = Log::CLogger>
        void logString(Hyprutils::CLI::eLogLevel level, const std::string& msg) {
            L& logger = *Log::logger;
            if constexpr (logTakesLoc<L>)
                logger.log(level, std::string_view{"hyprtail"}, std::string_view{msg});
            else
                logger.log(level, std::string_view{"[hyprtail] " + msg});
        }
    }

    // The "[hyprtail] " prefix is added here, callers don't write it.
    template <typename... Args>
    void log(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        detail::logString(level, std::format(fmt, std::forward<Args>(args)...));
    }

    // -------------------------------------------------------------------- GL

    // Array-buffer binding through Hyprland's cache when it has one, raw
    // otherwise. Plugin code must bind 0 again before deleting a buffer.
    template <typename GL = Render::GL::CHyprOpenGLImpl>
    void bindArrayBuffer(GLuint buffer) {
        GL& gl = *Render::GL::g_pHyprOpenGL;
        if constexpr (requires { gl.bindArrayBuffer(buffer); })
            gl.bindArrayBuffer(buffer);
        else
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
    }

    // ---------------------------------------------------------------- windows

    template <typename W>
    const std::string& windowClass(const W& w) {
        if constexpr (requires { w.metadata().appID(); })
            return w.metadata().appID();
        else
            return w.m_class;
    }

    template <typename W>
    const std::string& windowTitle(const W& w) {
        if constexpr (requires { w.metadata().title(); })
            return w.metadata().title();
        else
            return w.m_title;
    }

    // ---------------------------------------------------------------- hyprctl

    // Registers an exact-match `hyprctl <name>` command. `reply` gets true for
    // -j. Unregister with HyprlandAPI::unregisterHyprCtlCommand.
#ifdef HYPRTAIL_COMPAT_IPC_S1
    using StatusCommand = SP<IPC::Socket1::SCommand>;

    inline StatusCommand registerStatusCommand(HANDLE handle, const std::string& name, std::function<std::string(bool json)> reply) {
        using namespace IPC::Socket1;
        return HyprlandAPI::registerHyprCtlCommand(handle, SCommand{.name = name, .match = COMMAND_MATCH_EXACT, .handler = [reply = std::move(reply)](const SRequest& req) {
                                                                        return SResponse{reply(req.format == FORMAT_JSON)};
                                                                    }});
    }
#else
    using StatusCommand = SP<SHyprCtlCommand>;

    inline StatusCommand registerStatusCommand(HANDLE handle, const std::string& name, std::function<std::string(bool json)> reply) {
        return HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = name, .exact = true, .fn = [reply = std::move(reply)](eHyprCtlOutputFormat format, std::string) {
                                                                               return reply(format == eHyprCtlOutputFormat::FORMAT_JSON);
                                                                           }});
    }
#endif

}

#undef HYPRTAIL_COMPAT_IPC_S1
