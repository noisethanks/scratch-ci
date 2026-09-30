#pragma once

#include <vector>

#include <hyprutils/animation/AnimationManager.hpp>

#include "TrailBuffer.hpp"

// Defaults of the spring source's settings (SPEC §13.7 `source:<name>`).
// Stiffness and damping are per link and large because every point chases
// the one before it: the tail's lag adds up over the chain, so each link has
// to answer in a few ms for the whole chain to lag by a few hundred.
namespace spring {
    constexpr float MASS        = 1.F;
    constexpr float STIFFNESS   = 30000.F; // omega0 ~ 173 rad/s
    constexpr float DAMPING     = 200.F;   // damping ratio ~ 0.58
    constexpr float AGE_STEP_MS = 10.F;
}

// A chain of `capacity` points that chase each other (SPEC §13.7, source
// `spring`). The head point chases the pointer, every other point chases the
// one before it, each axis an independent spring (hyprutils advanceSpring).
// Unlike the pointer history nothing is appended: the points are fixed, so
// size() is the capacity once the chain has been seeded by the first insert.
//
// Point data for the shaders, in the ring's conventions so every geometry
// shader works unchanged: the oldest node is the tail and the newest the
// head, only the tail starts a segment, velocity is px/ms, distPx
// accumulates from the tail. A point has no birth of its own: point k is
// given birth time (activeMs - k * age_step_ms), where activeMs is the last
// time the chain was moving or the pointer inserted a target. So the chain
// is fresh while it moves, tapers toward the tail (the tail is older), and
// fades as one once it stops.
class CSpringChainSource final : public ISource {
  public:
    CSpringChainSource(size_t capacity, uint64_t seedBase);

    std::string_view   kind() const override;
    void               insert(const SVec2f& pos, double nowMs, bool segmentStart) override;
    void               tick(double nowMs, double dt) override;
    void               configure(const std::map<std::string, double>& values) override;
    void               resize(size_t capacity) override;
    void               clear() override;

    size_t             size() const override;
    size_t             capacity() const override;
    bool               empty() const override;
    const SCursorNode& newest() const override;
    uint64_t           generation() const override;
    double             newestBirthMs() const override;
    void               orderedCopy(std::vector<SGpuNode>& out, double refMs) const override;
    bool               needsContinuousUpload() const override;
    size_t             visibleCount(double nowMs, double fadeMs) const override;
    std::optional<STrailBounds> visibleBounds(double nowMs, double fadeMs, bool includeOlderNode = true) const override;
    bool               isSettled(double nowMs, double fadeMs) const override;

  private:
    struct SPoint {
        SVec2f pos;
        SVec2f vel; // px/s, the point's own velocity (the spring's state)
    };

    // Every point at pos, at rest.
    void reseed(const SVec2f& pos, double nowMs);

    // Rebuilds m_nodes from the points and activeMs, and bumps the generation.
    void rebuild();

    size_t                             m_capacity = 0;
    uint64_t                           m_seedBase = 0;
    bool                               m_seeded   = false; // false until the first insert (and after clear)
    std::vector<SPoint>                m_points;           // head first
    std::vector<SCursorNode>           m_nodes;            // head first, derived from m_points
    SVec2f                             m_target;           // where the head is chasing
    SCursorNode                        m_last{};           // the last insert, newest()
    double                             m_activeMs = 0.0;
    bool                               m_unsettled = false; // a point is still moving, or chasing a target it hasn't reached
    uint64_t                           m_generation = 0;
    Hyprutils::Animation::SSpringCurve m_curve;
    float                              m_ageStepMs = spring::AGE_STEP_MS;
};
