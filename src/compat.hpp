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
//
// Also here: compile-time checks that each hooked function still has the
// signature its detour is written for (the `hooks` namespace at the end).
// Those are not adapters. They make a Hyprland change the plugin cannot follow
// fail the build, which the reinterpret_cast hook installation would not.

#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <plugins/PluginAPI.hpp>
#include <debug/log/Logger.hpp>
#include <render/OpenGL.hpp>
#include <pointer/PointerManager.hpp>
#include <pointer/PointerController.hpp>

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

    // ------------------------------------------------------------------ hooks

    // Hooked functions (main.cpp installHooks). A detour is installed through
    // reinterpret_cast<void*>, so the compiler never compares its signature with
    // the target's: if Hyprland adds or changes a parameter, the plugin still
    // compiles and the detour then misreads every argument. These are the
    // signatures the detours are written for, `this` aside, and the checks
    // below fail the build when a target no longer has exactly one of them.
    //
    // Hyprland main added a leading Render::CRenderContext& to
    // renderSoftwareCursorsFor and saveBufferForMirror (NOTES "Main 579829f").
    // Porting means changing the alias here and the detour in main.cpp together;
    // main.cpp asserts that each detour matches its alias (DetourT).
    namespace hooks {
        using RenderSoftwareCursorsForSig = void(PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);
        using ControllerWarpToSig         = void(const Vector2D&, bool) const;
        using SaveBufferForMirrorSig      = bool(const CBox&);

        // Pointer to a member of C with signature Sig. A named alias because
        // clang-format mangles the bare `Sig C::*` spelling in declarations.
        template <typename C, typename Sig>
        using MemberPtr = Sig C::*;

        // The member function pointer types of the real targets.
        using RenderSoftwareCursorsFor = MemberPtr<Pointer::CPointerManager, RenderSoftwareCursorsForSig>;
        using ControllerWarpTo         = MemberPtr<Pointer::CPointerController, ControllerWarpToSig>;
        using SaveBufferForMirror      = MemberPtr<Render::GL::CHyprOpenGLImpl, SaveBufferForMirrorSig>;

        // The free-function type of the detour, and of the original called
        // through CFunctionHook::m_original: `this` becomes a void pointer.
        template <typename Pmf>
        struct Detour;
        template <typename R, typename C, typename... A>
        struct Detour<R (C::*)(A...)> {
            using type = R (*)(void*, A...);
        };
        template <typename R, typename C, typename... A>
        struct Detour<R (C::*)(A...) const> {
            using type = R (*)(const void*, A...);
        };
        template <typename Pmf>
        using DetourT = typename Detour<Pmf>::type;

        // static_cast of the address picks the overload with exactly this
        // type, and is ill-formed (so false here) when there is none: a changed
        // parameter list, a changed constness, or an overload set without it.
        // Templates on the class so tests/compat can check them against
        // stand-ins with other signatures.
        template <typename C>
        inline constexpr bool renderSoftwareCursorsForMatches = requires { static_cast<MemberPtr<C, RenderSoftwareCursorsForSig>>(&C::renderSoftwareCursorsFor); };
        template <typename C>
        inline constexpr bool controllerWarpToMatches = requires { static_cast<MemberPtr<C, ControllerWarpToSig>>(&C::warpTo); };
        template <typename C>
        inline constexpr bool saveBufferForMirrorMatches = requires { static_cast<MemberPtr<C, SaveBufferForMirrorSig>>(&C::saveBufferForMirror); };

        static_assert(renderSoftwareCursorsForMatches<Pointer::CPointerManager>,
                      "hyprtail: CPointerManager::renderSoftwareCursorsFor no longer has the signature hkRenderSoftwareCursorsFor (main.cpp) is written for. The detour is "
                      "installed through reinterpret_cast, so the compiler cannot check it and it would misread its arguments. Port the detour and hooks::RenderSoftwareCursorsFor "
                      "in src/compat.hpp together (NOTES \"Main 579829f\").");
        static_assert(controllerWarpToMatches<Pointer::CPointerController>,
                      "hyprtail: CPointerController::warpTo no longer has the signature hkControllerWarpTo (main.cpp) is written for. The detour is installed through "
                      "reinterpret_cast, so the compiler cannot check it and it would misread its arguments. Port the detour and hooks::ControllerWarpTo in src/compat.hpp "
                      "together.");
        static_assert(saveBufferForMirrorMatches<Render::GL::CHyprOpenGLImpl>,
                      "hyprtail: CHyprOpenGLImpl::saveBufferForMirror no longer has the signature hkSaveBufferForMirror (main.cpp) is written for. The detour is installed "
                      "through reinterpret_cast, so the compiler cannot check it and it would misread its arguments. Port the detour and hooks::SaveBufferForMirror in "
                      "src/compat.hpp together (NOTES \"Main 579829f\").");
    }

}

#undef HYPRTAIL_COMPAT_IPC_S1
