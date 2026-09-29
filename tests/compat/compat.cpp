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

// Instantiates every wrapper with the argument shapes the plugin uses.
[[maybe_unused]] static void instantiateAll(const CWindow& window, HANDLE handle) {
    hyprtail::compat::log(Log::INFO, "no arguments");
    hyprtail::compat::log(Log::INFO, "{} {}", 1, std::string{"two"});
    // The call that compiles unchecked against Hyprland's own logger on main.
    hyprtail::compat::log(Log::ERR, "can't write {}", std::filesystem::path{"x"}.string());

    hyprtail::compat::bindArrayBuffer(0);

    [[maybe_unused]] const std::string& cls   = hyprtail::compat::windowClass(window);
    [[maybe_unused]] const std::string& title = hyprtail::compat::windowTitle(window);

    hyprtail::compat::StatusCommand command = hyprtail::compat::registerStatusCommand(handle, "x", [](bool json) { return std::string{json ? "{}" : ""}; });
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);
}
