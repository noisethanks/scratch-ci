local PRIMARY_NAME = "WAYLAND-1" -- CONFIRM THIS

local PRIMARY_MODE = "1280x720@60"
local SECOND_MODE  = "1280x720@60"

-- Primary already exists when config loads, no race, plain call.
hl.monitor({ output = PRIMARY_NAME, mode = PRIMARY_MODE, position = "0x0", scale = 1 })

local mainMod      = "ALT"
local mainModShift = "ALT + SHIFT"
hl.bind(mainMod .. " + Q", hl.dsp.window.close(), { description = "Kill Active Window" })

local terminal = "ghostty"
hl.bind(mainMod .. " + L", hl.dsp.exec_cmd(terminal),
    { description = "New Terminal" })

hl.config({
    cursor = {
        no_hardware_cursors = 1,
        hide_on_key_press = 1
    },
    input = {
        follow_mouse = 1,
    },
    debug = {
        disable_logs = false
    }
})

-- hyprtail settings (SPEC section 9, config surface v2 as of §13.16 phase 4).
-- Defaults shown, commented. With the plugin loaded by hand (make load)
-- rather than hl.plugin.load, these keys are unknown until it loads, so
-- Hyprland shows a config error until then; loading the plugin triggers a
-- reload that clears it.
--
-- classic (not the default, "subtle" is) has the idle ring layer, off by
-- default in that preset's own manifest (hyprtail/presets/classic.conf); turn
-- it on here with a params override rather than editing the preset, so this
-- file is the one thing that needs changing to get the idle layer active
-- for nested testing.
hl.config({
    plugin = {
        hyprtail = {
            trail             = "prefab:classic",
            params            = "idle:enabled=true",
            --             capacity          = 64,
            --             min_spacing       = 2,
            --             warp              = "break",
            --             damage_padding    = 0,
            --             layer1_vertex     = "",  -- "" = the preset's own shader; relative = against ~/.config/hypr/hyprtail/; ~ works
            --             layer1_fragment   = "",
            --             layer2_vertex     = "",
            --             layer2_fragment   = "",
        },
    },
})

hl.on("hyprland.start", function()
    -- "second" doesn't exist until this runs, has to be created and
    -- configured here, unlike the primary above.
    hl.exec_cmd(
        "hyprctl output create wayland second && "
        .. "sleep 0.2 && "
        .. "hyprctl eval 'hl.monitor({ output = \"second\", mode = \""
        .. SECOND_MODE
        .. "\", position = \"1280x0\", scale = 1, mirror = \"WAYLAND-1\" })'"
    )
end)
