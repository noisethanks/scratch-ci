
-- ---- hyprtail smoke test (tests/hyprtester/smoke.lua) ----
-- `make smoke` appends this to hyprtester/test.lua (same directory, so its
-- relative requires keep working). Plugin keys are unknown until the plugin
-- loads, so this reports a config error at startup; the reload after
-- `/plugin load` (PluginSystem.cpp:135) applies it.

-- Output created and removed by the hotplug rounds (hyprtail_smoke.cpp),
-- far right of every other output. Without this rule it would get the
-- catch-all rule above, which disables unknown outputs.
hl.monitor({ output = "HYPRTAIL-TEST", mode = "1280x720@60", position = "20000x0", scale = "1" })

hl.config({
    plugin = {
        hyprtail = {
            fade_ms           = 500, -- FADE_MS in hyprtail_smoke.cpp
            warp              = "line", -- the test moves by warps; connect them into a ribbon
            idle_enabled      = true,
            idle_delay_ms     = 50,
            idle_duration_ms  = 200,
        },
    },
})

-- No Xwayland: the test doesn't need it, and it would claim an X display in
-- the shared /tmp/.X11-unix.
hl.config({ xwayland = { enabled = false } })
