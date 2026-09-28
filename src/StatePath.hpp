#pragma once

// Hyprland-free (no Hyprland headers): shared by Diagnostics.cpp and
// CrashGuard.cpp, both of which write into the same per-user state
// directory. Kept header-only since it's a handful of lines with no
// meaningful implementation to hide.

#include <cstdlib>
#include <filesystem>

namespace hyprtail {
    // $XDG_STATE_HOME/hyprtail, else ~/.local/state/hyprtail. Empty path if
    // neither env var resolves to an absolute path.
    inline std::filesystem::path stateDir() {
        if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && xdg[0] == '/')
            return std::filesystem::path{xdg} / "hyprtail";

        if (const char* home = std::getenv("HOME"); home && home[0] == '/')
            return std::filesystem::path{home} / ".local" / "state" / "hyprtail";

        return {};
    }
}
