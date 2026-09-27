#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include <plugins/PluginAPI.hpp>

// User settings (SPEC section 9), registered through the V2 config API
// (HyprlandAPI::addConfigValueV2), which works with both the Lua and the
// legacy hyprlang config provider. V1 addConfigValue/getConfigValue only work
// with the legacy provider at the pinned commit.
//
// Lua:      hl.config({ plugin = { hyprtail = { capacity = 128 } } })
// hyprlang: plugin:hyprtail:capacity = 128
namespace hyprtail::cfg {
    struct SValues {
        std::string preset          = "subtle"; // SPEC §13.7
        size_t      capacity        = 64;
        float       minSpacingPx    = 2.F;
        // Kept despite SPEC §13.8's literal "Removed" list: its replacement
        // (`warp = "break"|"line"|"curve"`, §13.10) is phase 6, not built.
        // Removing this now, with nothing to replace it, would delete the
        // "connect the trail across warps" feature outright until phase 6
        // ships -- not a rename, a regression. See NOTES "Phase 4".
        bool        interpolateWarps = false;
        float       damagePaddingPx  = 0.F; // on top of stock extent and shader-declared padding

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

    // Shader path from the config: "" -> empty (built-in); "~" / "~/..." ->
    // $HOME; relative -> against the Hyprland config directory
    // ($XDG_CONFIG_HOME/hypr, else ~/.config/hypr).
    std::filesystem::path resolveShaderPath(const std::string& configured);
}
