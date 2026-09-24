#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <plugins/PluginAPI.hpp>

// User settings (SPEC section 9), registered through the V2 config API
// (HyprlandAPI::addConfigValueV2), which works with both the Lua and the
// legacy hyprlang config provider. V1 addConfigValue/getConfigValue only work
// with the legacy provider at the pinned commit.
//
// Lua:      hl.config({ plugin = { hyprtail = { fade_ms = 400 } } })
// hyprlang: plugin:hyprtail:fade_ms = 400
namespace hyprtail::cfg {
    struct SValues {
        double      fadeMs           = 500.0;
        float       widthPx          = 8.F;
        size_t      capacity         = 64;
        float       minSpacingPx     = 2.F;
        float       miterLimit       = 2.F;
        bool        interpolateWarps = false;
        float       damagePaddingPx  = 0.F; // on top of stock extent and shader-declared padding
        uint64_t    colorSlow        = 0xFF1A66FF; // ARGB, sRGB; stock palette at rest speed
        uint64_t    colorFast        = 0xFFFF1A1A; // ARGB, sRGB; stock palette at speedRef and above
        std::string vertexShader;           // "" = built-in
        std::string fragmentShader;         // "" = built-in

        // Idle/presence slot (SPEC section 7).
        bool        idleEnabled        = false;
        double      idleDelayMs        = 500.0;
        double      idleDurationMs     = 1500.0; // 0 = until the pointer moves
        float       idleRadiusPx       = 24.F;
        bool        idleWhenHidden     = false;
        std::string idleVertexShader;   // "" = built-in
        std::string idleFragmentShader; // "" = built-in
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
