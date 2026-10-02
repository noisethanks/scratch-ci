#pragma once

#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

// Hyprland-free parsing and bounds for the emit settings (SPEC §13.9), kept
// out of Config.cpp so tests/unit can link them without the compositor.
namespace hyprtail::cfg {
    struct SPair {
        double x = 0.0, y = 0.0;
        bool   operator==(const SPair&) const = default;
    };

    // emit_offset bound, logical px, per component (inclusive). A nudge
    // relative to the cursor image, not a placement: typical cursors are
    // 24-48 logical px and large accessibility ones about 128, so 128 covers
    // a shift of a full large cursor's width without allowing arbitrary
    // placement.
    inline constexpr double EMIT_OFFSET_MAX_PX = 128.0;

    // "x y": two whitespace-separated finite numbers and nothing else, same
    // grammar as the Lua vec2 string form (LuaConfigVec2.cpp:14-25).
    inline std::optional<SPair> parseTwoFloats(std::string_view text) {
        std::istringstream in{std::string{text}};
        double             x = 0.0, y = 0.0;
        std::string        tail;
        if (!(in >> x >> y) || (in >> tail) || !std::isfinite(x) || !std::isfinite(y))
            return std::nullopt;
        return SPair{x, y};
    }

    // emit_from's "fx fy" form: both components in [0, 1], inclusive.
    inline bool emitFromInRange(const SPair& p) {
        return p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0;
    }

    // emit_offset: finite, each component within +-EMIT_OFFSET_MAX_PX.
    inline bool emitOffsetInRange(double x, double y) {
        return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= EMIT_OFFSET_MAX_PX && std::abs(y) <= EMIT_OFFSET_MAX_PX;
    }

    // The value emit_offset resolves to: the read value when in range, else
    // `previous` (the caller reports the rejection). On the first parse
    // `previous` is the default {0, 0}.
    inline SPair resolveEmitOffset(double x, double y, const SPair& previous) {
        return emitOffsetInRange(x, y) ? SPair{x, y} : previous;
    }
}
