#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <helpers/math/Math.hpp>

#include "Diagnostics.hpp"
#include "ShaderSlot.hpp"

// `hyprctl hyprtail` (SPEC §13.13): a snapshot of the plugin's state, taken
// on the main thread by the command handler, and its text / JSON rendering.
namespace hyprtail::status {
    struct SLayer {
        std::string                                      name;
        bool                                             enabled = false, disabled = false, resolved = false;
        double                                           fadeMs = 0.0, startMs = 0.0, durationMs = 0.0;
        float                                            extentPx = 0.F;
        std::vector<std::pair<std::string, std::string>> params; // name, value
        SSlotStatus                                      shader;
    };

    struct SMonitor {
        std::string                      name;
        uint64_t                         renders      = 0; // renders of this monitor seen (RENDER_BEGIN)
        uint64_t                         hookRuns     = 0; // lifecycle run from the cursor hook
        uint64_t                         fallbackRuns = 0; // lifecycle run from RENDER_LAST_MOMENT
        uint64_t                         draws        = 0; // renders that drew at least one layer
        uint64_t                         emptySkips   = 0; // renders without damage (workspace skipped)
        std::vector<std::optional<CBox>> layerBoxes;       // per layer: last drawn box, logical monitor-local

        // Screenshare exclude (SPEC §13.12).
        bool needsCopyFB     = false; // needsACopyFB() as of the last render: mirrored, or captured
        bool captureFallback = false; // the capture hook missed a render here; not drawing while it needs a copy
    };

    struct SSnapshot {
        std::string rev, builtHash, runningHash, preset, screenshare;
        bool        cursorHook = false, warpHook = false, captureHook = false;
        uint64_t    renders    = 0;

        struct {
            size_t      nodes = 0, capacity = 0;
            uint64_t    generation   = 0;
            bool        pendingBreak = false;
            std::string warpMode     = "break";
            bool        gpuFailed    = false;
            double      stillMs      = 0.0;
        } source;

        std::vector<SLayer>   layers;
        std::vector<SMonitor> monitors;
        diag::SStats          diag;
        std::string           errorFile;
    };

    std::string text(const SSnapshot& s);
    std::string json(const SSnapshot& s);
}
