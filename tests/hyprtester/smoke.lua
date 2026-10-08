
-- ---- hyprtail smoke test (tests/hyprtester/smoke.lua) ----
-- `make smoke` appends this to hyprtester/test.lua (same directory, so its
-- relative requires keep working). Plugin keys are unknown until the plugin
-- loads, so this reports a config error at startup; the reload after
-- `/plugin load` (PluginSystem.cpp:135) applies it.

-- Output created and removed by the hotplug rounds (hyprtail_smoke.cpp),
-- far right of every other output. Without this rule it would get the
-- catch-all rule above, which disables unknown outputs.
hl.monitor({ output = "HYPRTAIL-TEST", mode = "1280x720@60", position = "20000x0", scale = "1" })

-- Plugin settings the test changes while it runs (the instanced-topology
-- rounds in hyprtail_smoke.cpp): it writes "trail=", "params=" and
-- "capacity=" lines to $XDG_STATE_HOME/hyprtail-smoke-plugin.conf and sends
-- /reload, which runs this file again. No file: the defaults below.
local function plugin_settings()
    local settings = {
        -- ink has the idle ring layer, off in the preset itself; the
        -- params string turns it on with a short delay and duration.
        -- ink:fade_ms is FADE_MS in hyprtail_smoke.cpp.
        trail    = "prefab:ink",
        params   = "idle:enabled=true idle:start_ms=50 idle:duration_ms=200 ink:fade_ms=500",
        capacity = nil,
    }

    local state = os.getenv("XDG_STATE_HOME")
    local file  = state and io.open(state .. "/hyprtail-smoke-plugin.conf", "r")
    if file then
        for line in file:lines() do
            local key, value = line:match("^(%w+)=(.*)$")
            if key == "trail" or key == "params" then
                settings[key] = value
            elseif key == "capacity" then
                settings.capacity = tonumber(value)
            end
        end
        file:close()
    end
    return settings
end

local settings = plugin_settings()

hl.config({
    plugin = {
        hyprtail = {
            trail    = settings.trail,
            params   = settings.params,
            capacity = settings.capacity, -- nil (unset) keeps the plugin default
            warp     = "line", -- the test moves by warps; connect them into a ribbon
        },
    },
})

-- No Xwayland: the test doesn't need it, and it would claim an X display in
-- the shared /tmp/.X11-unix.
hl.config({ xwayland = { enabled = false } })
