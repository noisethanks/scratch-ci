#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "TrailBuffer.hpp"

// Warp interpolation (SPEC §13.10): the nodes a hooked warp inserts between
// the trail's newest node and the warp target, as positions and birth times.
// Generation only: the caller decides when each node goes into the source
// (today all at once, at warp time). No Hyprland headers, so the easing, the
// timing and the landing are unit tested with plain functions standing in
// for Hyprland's bezier curves.
namespace hyprtail::warp {
    enum class eShape : uint8_t {
        LINE,  // straight from the newest node to the target
        CURVE, // quadratic Bezier, control point along the incoming velocity
    };

    // Normalized elapsed time (0..1) -> normalized progress along the path.
    // Empty = linear, today's spacing. Results outside 0..1 are overshoot and
    // are kept, up to 4 path lengths either side; a non-finite result counts
    // as linear for that node.
    using FEase = std::function<float(float)>;

    struct SNode {
        SVec2f posPx;
        double birthMs;
    };

    // Nodes for a warp from `from` (the newest node before the warp, a copy)
    // to `to` at nowMs. Count: chord / minSpacingPx, at least 1, at most a
    // quarter of `capacity`.
    //
    // Birth times (warp_ms): node i of n is born at start + (nowMs - start) *
    // i / n, start = max(from's birth, nowMs - warpMs). So the window is
    // warpMs however long the pointer rested before, shorter only when the
    // newest node is younger than that, and never before it: births never
    // decrease along the source, which its visibility walk relies on
    // (TrailBuffer.hpp, visibleCountOf). Every birth is within [start,
    // max(start, nowMs)], the last one exactly the upper end. Real time,
    // never eased: the easing moves each node along the path instead.
    //
    // The last node is `to` exactly, whatever the easing.
    std::vector<SNode> nodes(eShape shape, const SCursorNode& from, const SVec2f& to, double nowMs, double warpMs, float minSpacingPx, size_t capacity, const FEase& ease);
}
