local PRIMARY_NAME = "WAYLAND-1"  -- CONFIRM THIS

local PRIMARY_MODE = "1280x720@60"
local SECOND_MODE  = "1280x720@60"

-- Primary already exists when config loads, no race, plain call.
hl.monitor({ output = PRIMARY_NAME, mode = PRIMARY_MODE, position = "0x0", scale = 1 })

local mainMod      = "ALT"
local mainModShift = "ALT + SHIFT"
hl.bind(mainMod .. " + Q", hl.dsp.window.close(), { description = "Kill Active Window" })

local terminal     = "ghostty"
hl.bind(mainMod .. " + L", hl.dsp.exec_cmd(terminal),
    { description = "New Terminal" })

hl.config({
    cursor ={
        no_hardware_cursors= 1,
        hide_on_key_press = 1
    },
    input = {
        follow_mouse                = 1,
    },
    debug ={
        disable_logs = false
    }
})

hl.on("hyprland.start", function()
    -- "second" doesn't exist until this runs, has to be created and
    -- configured here, unlike the primary above.
    -- hl.exec_cmd(
    --     "hyprctl output create wayland second && "
    --     .. "sleep 0.2 && "
    --     .. "hyprctl eval 'hl.monitor({ output = \"second\", mode = \""
    --     .. SECOND_MODE
    --     .. "\", position = \"1280x0\", scale = 1 })'"
    -- )
end)
