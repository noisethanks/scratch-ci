#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <helpers/math/Math.hpp>

#include "Diagnostics.hpp"
#include "ShaderSlot.hpp"

// `hyprctl hyprtail` (SPEC §13.13): a snapshot of the plugin's state, taken
// on the main thread by the command handler, and its text / JSON rendering.
namespace hyprtail::status {
    struct SMonitor {
        std::string         name;
        uint64_t            renders      = 0; // renders of this monitor seen (RENDER_BEGIN)
        uint64_t            hookRuns     = 0; // lifecycle run from the cursor hook
        uint64_t            fallbackRuns = 0; // lifecycle run from RENDER_LAST_MOMENT
        uint64_t            trailDraws   = 0; // trail pass elements added
        uint64_t            idleDraws    = 0; // idle pass elements added
        uint64_t            emptySkips   = 0; // renders without damage (workspace skipped)
        std::optional<CBox> trailBox, idleBox; // last drawn box, logical monitor-local
    };

    struct SSnapshot {
        std::string rev, builtHash, runningHash;
        bool        cursorHook = false, warpHook = false;
        uint64_t    renders    = 0;

        struct {
            bool         disabled = false;
            size_t       nodes = 0, capacity = 0;
            uint64_t     generation       = 0;
            bool         pendingBreak     = false;
            bool         interpolateWarps = false;
            double       fadeMs           = 0.0;
            SSlotStatus  shader;
        } trail;

        struct {
            bool        enabled = false, disabled = false, showing = false;
            double      stillMs = 0.0;
            SSlotStatus shader;
        } idle;

        std::vector<SMonitor> monitors;
        diag::SStats          diag;
        std::string           errorFile;
    };

    std::string text(const SSnapshot& s);
    std::string json(const SSnapshot& s);
}
