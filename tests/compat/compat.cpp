// Compile-only check of src/compat.hpp against whichever Hyprland headers the
// build selects (`make test-compat`, see the Makefile). Passing means: every
// wrapper instantiates, and the assumptions compat.hpp relies on about
// Hyprland hold in this checkout. Nothing here is ever run or linked.

#include <filesystem>
#include <string>

#include "../../src/compat.hpp"

using CWindow = Desktop::View::CWindow;

// Detection is by what a type offers, so it must not be able to see both or
// neither: the wrapper would then silently pick one.
template <typename W>
constexpr bool hasMetadata = requires(const W& w) {
    w.metadata().appID();
    w.metadata().title();
};
template <typename W>
constexpr bool hasMembers = requires(const W& w) {
    w.m_class;
    w.m_title;
};
static_assert(hasMetadata<CWindow> != hasMembers<CWindow>, "CWindow must offer exactly one of metadata() and m_class/m_title");

// compat probes the location overload of CLogger::log(). The LOG macro is the
// other half of the same Hyprland change; if they ever separate, the probe is
// what to trust and this is the reminder to look.
#ifdef LOG
static_assert(hyprtail::compat::detail::logTakesLoc<Log::CLogger>, "LOG exists but CLogger::log() has no location overload");
#else
static_assert(!hyprtail::compat::detail::logTakesLoc<Log::CLogger>, "CLogger::log() has a location overload but there is no LOG");
#endif

// The hook guards in compat.hpp (namespace hooks) pass for the selected
// checkout's real classes, or including compat.hpp would not compile. This
// checks they can also say no: each stand-in below has the signature Hyprland
// main has since 579829f (a leading Render::CRenderContext& on
// renderSoftwareCursorsFor and saveBufferForMirror, PointerManager.hpp:67 and
// OpenGL.hpp:228 there) or a changed qualifier, and must be rejected.
namespace Render {
    class CRenderContext;
}
struct SStandInCursorWithContext {
    void renderSoftwareCursorsFor(Render::CRenderContext&, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D> = {}, bool = false, bool = false);
};
struct SStandInSaveWithContext {
    bool saveBufferForMirror(Render::CRenderContext&, const CBox&);
};
struct SStandInWarpNotConst {
    void warpTo(const Vector2D&, bool = false);
};
struct SStandInSaveMatching {
    bool saveBufferForMirror(const CBox&);
};
struct SStandInCursorMatching {
    void renderSoftwareCursorsFor(PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D> = {}, bool = false, bool = false);
};
struct SStandInWarpMatching {
    void warpTo(const Vector2D&, bool = false) const;
};
static_assert(!hyprtail::compat::hooks::renderSoftwareCursorsForMatches<SStandInCursorWithContext>, "the guard accepted a renderSoftwareCursorsFor with a leading context");
static_assert(!hyprtail::compat::hooks::saveBufferForMirrorMatches<SStandInSaveWithContext>, "the guard accepted a saveBufferForMirror with a leading context");
static_assert(!hyprtail::compat::hooks::controllerWarpToMatches<SStandInWarpNotConst>, "the guard accepted a non-const warpTo");
static_assert(hyprtail::compat::hooks::saveBufferForMirrorMatches<SStandInSaveMatching>, "the guard rejected a saveBufferForMirror with the expected signature");
static_assert(hyprtail::compat::hooks::renderSoftwareCursorsForMatches<SStandInCursorMatching>, "the guard rejected a renderSoftwareCursorsFor with the expected signature");
static_assert(hyprtail::compat::hooks::controllerWarpToMatches<SStandInWarpMatching>, "the guard rejected a warpTo with the expected signature");

// Instantiates every wrapper with the argument shapes the plugin uses.
[[maybe_unused]] static void instantiateAll(const CWindow& window, HANDLE handle) {
    hyprtail::compat::log(Log::INFO, "no arguments");
    hyprtail::compat::log(Log::INFO, "{} {}", 1, std::string{"two"});
    // The call that compiles unchecked against Hyprland's own logger on main.
    hyprtail::compat::log(Log::ERR, "can't write {}", std::filesystem::path{"x"}.string());

    hyprtail::compat::bindArrayBuffer(0);

    [[maybe_unused]] const std::string& cls   = hyprtail::compat::windowClass(window);
    [[maybe_unused]] const std::string& title = hyprtail::compat::windowTitle(window);

    hyprtail::compat::StatusCommand     command = hyprtail::compat::registerStatusCommand(handle, "x", [](bool json) { return std::string{json ? "{}" : ""}; });
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);
}
