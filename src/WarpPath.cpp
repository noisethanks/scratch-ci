#include "WarpPath.hpp"

#include <algorithm>
#include <cmath>

namespace hyprtail::warp {
    namespace {
        // How far eased progress may leave 0..1, in path lengths. Keeps every
        // node finite and the damage box sane whatever the curve returns.
        // Never reached by a curve from hl.curve: its control points are
        // limited to -1..2 (LuaBindingsConfigRules.cpp:322), which keeps y
        // within about -0.66..1.66. The legacy `bezier` keyword has no limit
        // (legacy ConfigManager.cpp:1389-1421).
        constexpr float MAX_OVERSHOOT = 4.F;

        // Progress s along the straight path. Outside 0..1 it carries on along
        // the same line: past the target (s > 1) or behind the start (s < 0).
        SVec2f lineAt(const SVec2f& p0, const SVec2f& p2, float s) {
            return {p0.x + (p2.x - p0.x) * s, p0.y + (p2.y - p0.y) * s};
        }

        // Progress s along the quadratic Bezier (p0, p1, p2), s being its
        // parameter, not arc length. Outside 0..1 it carries on along the
        // tangent at the nearer end (B'(1) = 2 (p2 - p1), B'(0) = 2 (p1 -
        // p0)), not along the parabola, which would bend an overshoot back
        // around instead of past the target in the direction of travel.
        SVec2f curveAt(const SVec2f& p0, const SVec2f& p1, const SVec2f& p2, float s) {
            if (s > 1.F) {
                const float k = 2.F * (s - 1.F);
                return {p2.x + (p2.x - p1.x) * k, p2.y + (p2.y - p1.y) * k};
            }
            if (s < 0.F) {
                const float k = 2.F * s;
                return {p0.x + (p1.x - p0.x) * k, p0.y + (p1.y - p0.y) * k};
            }
            // The expression `warp = "curve"` used before easing existed,
            // kept as is so the linear case reproduces it bit for bit.
            const float u = 1.F - s;
            return {u * u * p0.x + 2.F * u * s * p1.x + s * s * p2.x, u * u * p0.y + 2.F * u * s * p1.y + s * s * p2.y};
        }
    }

    std::vector<SNode> nodes(eShape shape, const SCursorNode& from, const SVec2f& to, double nowMs, float minSpacingPx, size_t capacity, const FEase& ease) {
        const SVec2f p0    = from.posPx;
        const SVec2f p2    = to;
        const float  chord = std::hypot(p2.x - p0.x, p2.y - p0.y);

        // Control point along the incoming velocity, for tangent continuity
        // at p0. Zero velocity (a fresh segment) falls back to the chord's
        // midpoint, which makes the quadratic Bezier degenerate to a straight
        // line -- no special-casing needed. Unused by LINE.
        SVec2f p1 = {(p0.x + p2.x) / 2.F, (p0.y + p2.y) / 2.F};
        if (const float speed = std::hypot(from.velocity.x, from.velocity.y); speed > 1e-6F)
            p1 = {p0.x + from.velocity.x / speed * chord * 0.5F, p0.y + from.velocity.y / speed * chord * 0.5F};

        // Length / min_spacing, capped at a quarter of the capacity (SPEC
        // §13.10). Clamped as a double so a huge chord can't overflow the int.
        const double       cap  = std::max(1.0, static_cast<double>(capacity / 4));
        const int          n    = static_cast<int>(std::clamp(static_cast<double>(std::round(chord / std::max(minSpacingPx, 0.01F))), 1.0, cap));
        const double       t0   = from.birthTimeMs;
        const double       span = std::max(0.0, nowMs - t0);

        std::vector<SNode> out;
        out.reserve(n);
        for (int i = 1; i <= n; ++i) {
            const float t   = static_cast<float>(i) / static_cast<float>(n);
            SVec2f      pos = p2; // the last node lands on the target exactly
            if (i < n) {
                float s = t;
                if (ease) {
                    if (const float e = ease(t); std::isfinite(e))
                        s = std::clamp(e, -MAX_OVERSHOOT, 1.F + MAX_OVERSHOOT);
                }
                pos = shape == eShape::LINE ? lineAt(p0, p2, s) : curveAt(p0, p1, p2, s);
            }
            // Births spread evenly between the previous node's birth and now,
            // so the fade sweeps along the path in real time.
            out.push_back({pos, t0 + span * t});
        }
        return out;
    }
}
