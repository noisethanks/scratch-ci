#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <plugins/PluginAPI.hpp>
#include <helpers/math/Math.hpp>

// User settings (SPEC section 9), registered through the V2 config API
// (HyprlandAPI::addConfigValueV2), which works with both the Lua and the
// legacy hyprlang config provider. V1 addConfigValue/getConfigValue only work
// with the legacy provider at the pinned commit.
//
// Lua:      hl.config({ plugin = { hyprtail = { capacity = 128 } } })
// hyprlang: plugin:hyprtail:capacity = 128
namespace hyprtail::cfg {
    // SPEC §13.10. Replaces `interpolate_warps` (false = break, true = line);
    // the phase-6 replacement this key's removal was deferred to has shipped.
    enum class eWarpMode : uint8_t {
        BREAK, // default: a warp starts a new segment
        LINE,  // connects with a straight sweep (today's interpolate_warps = true)
        CURVE, // connects with nodes along a quadratic Bezier
    };

    const char* warpModeName(eWarpMode m);

    struct SValues {
        std::string trail           = "builtin:ink"; // SPEC §13.7
        size_t      capacity        = 64;
        float       minSpacingPx    = 2.F;
        eWarpMode   warp            = eWarpMode::BREAK;
        std::string warpBezier      = "";  // hl.curve name timing a line/curve warp (SPEC §13.10), "" = linear; looked up per warp
        float       damagePaddingPx = 0.F; // on top of stock extent and shader-declared padding

        // Emit offset (SPEC §13.9). nullopt = "hotspot" (default, today's
        // behavior); otherwise a position normalized to the cursor image
        // box (CPointerManager::getCursorBoxGlobal). emitOffsetPx (logical
        // px) is added after, always.
        std::optional<Vector2D> emitFromNorm;
        Vector2D                emitOffsetPx{0.0, 0.0};

        // Per-layer shader overrides, static keys indexed by the layer's
        // position in the preset's layer list (SPEC §13.7): layer1_vertex
        // .. layer4_fragment. "" = the preset's own shader for that stage.
        // An index the current preset has no layer for is a plugin warning.
        std::array<std::string, 4> layerVertex, layerFragment;

        // "<layer>:<name>=<value> ..." parameter overrides (SPEC §13.5),
        // validated against each layer's shader-declared params.
        std::string params;

        // Screenshare exclude (SPEC §13.12): "exclude" (default) or
        // "include" (today's behavior). A bad value warns and keeps the
        // previous one, same as the numeric settings.
        std::string screenshare = "exclude";
    };

    // Register all values. Call in PLUGIN_INIT. Returns false if any failed
    // (reported); the plugin then runs on built-in defaults for those.
    bool registerValues(HANDLE handle);

    // Release our references (PLUGIN_EXIT / teardown).
    void releaseValues();

    // Current values. Invalid ones are reported (config:<key>) and replaced by
    // the corresponding field of `previous`.
    SValues read(const SValues& previous);

    // hyprtail's config root: $XDG_CONFIG_HOME/hypr/hyprtail, else
    // ~/.config/hypr/hyprtail. Empty if neither variable is usable.
    std::filesystem::path hyprtailRoot();

    // The one resolver for every path hyprtail reads from the user's config
    // (shader paths from the config or a preset manifest, and the `trail`
    // setting's preset file): "" -> empty (built-in); "~" / "~/..." -> $HOME;
    // relative -> against hyprtailRoot() (deliberately not the Hyprland
    // config directory, unlike decoration:screen_shader: one base for every
    // hyprtail path).
    std::filesystem::path resolveShaderPath(const std::string& configured);
}
