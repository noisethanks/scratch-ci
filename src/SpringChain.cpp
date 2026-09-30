#include "SpringChain.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <hyprutils/animation/Spring.hpp>

namespace {
    // A point is at rest when it is this close to its target and this slow,
    // the same rule core applies to its own springs (hyprutils
    // AnimatedVariable.cpp:129-137), at a scale of pixels: a hundredth of a
    // pixel is invisible.
    constexpr float  SETTLE_POS_PX   = 0.05F;
    constexpr float  SETTLE_VEL_PXS  = 2.F;

    // Longest step one tick integrates. advanceSpring is exact for a fixed
    // target, so this isn't about stability: after a gap with no renders the
    // chain would otherwise cross the whole distance to the pointer in one
    // step instead of following it.
    constexpr double MAX_DT_MS = 33.0;

    // One axis of one point, reframed so the spring's rest position is the
    // fixed 1.0 advanceSpring solves around: value = 1 + (pos - target), so
    // value - 1 is the displacement from the target. The velocity is the
    // point's own (the target is constant over the step), carried as is.
    void advanceAxis(float& pos, float& vel, float target, float dtSec, const Hyprutils::Animation::SSpringCurve& curve) {
        float value = 1.F + (pos - target);
        Hyprutils::Animation::advanceSpring(value, vel, curve, std::chrono::duration<float>(dtSec));
        pos = target + (value - 1.F);
    }

    bool finite(const SVec2f& v) {
        return std::isfinite(v.x) && std::isfinite(v.y);
    }
}

CSpringChainSource::CSpringChainSource(size_t capacity, uint64_t seedBase) : m_capacity(std::max<size_t>(capacity, 1)), m_seedBase(seedBase) {
    m_curve.mass      = spring::MASS;
    m_curve.stiffness = spring::STIFFNESS;
    m_curve.damping   = spring::DAMPING;
}

std::string_view CSpringChainSource::kind() const {
    return "spring";
}

void CSpringChainSource::reseed(const SVec2f& pos, double nowMs) {
    m_points.assign(m_capacity, SPoint{.pos = pos, .vel = {}});
    m_target    = pos;
    m_seeded    = true;
    m_activeMs  = nowMs;
    m_unsettled = false;
    m_last      = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = {}, .distPx = 0.0, .seed = nodeSeed(m_seedBase, 0), .segmentStart = true};
    rebuild();
}

void CSpringChainSource::insert(const SVec2f& pos, double nowMs, bool segmentStart) {
    // A break detaches the trail from what came before. A chain has one
    // connected body, so there is nothing to fade: it restarts at pos.
    if (!m_seeded || segmentStart) {
        reseed(pos, nowMs);
        return;
    }

    SVec2f       velocity{};
    const double dt = nowMs - m_last.birthTimeMs;
    if (dt > 0.0)
        velocity = {static_cast<float>((pos.x - m_last.posPx.x) / dt), static_cast<float>((pos.y - m_last.posPx.y) / dt)};
    m_last = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity, .distPx = 0.0, .seed = m_last.seed, .segmentStart = false};

    // Only the head's target moves; the rest follow on the next tick.
    m_target    = pos;
    m_activeMs  = nowMs;
    m_unsettled = true;
    rebuild();
}

void CSpringChainSource::tick(double nowMs, double dtMs) {
    if (!m_seeded)
        return;

    const float dt = static_cast<float>(std::clamp(dtMs, 0.0, MAX_DT_MS) / 1000.0);

    bool        changed   = false;
    bool        unsettled = false;
    SVec2f      target    = m_target; // the head chases the pointer, each next point the one before, already advanced
    for (auto& p : m_points) {
        const SPoint before = p;

        if (dt > 0.F) {
            advanceAxis(p.pos.x, p.vel.x, target.x, dt, m_curve);
            advanceAxis(p.pos.y, p.vel.y, target.y, dt, m_curve);
        }

        bool settled = std::abs(p.pos.x - target.x) <= SETTLE_POS_PX && std::abs(p.pos.y - target.y) <= SETTLE_POS_PX && std::abs(p.vel.x) <= SETTLE_VEL_PXS &&
            std::abs(p.vel.y) <= SETTLE_VEL_PXS;
        if (!finite(p.pos) || !finite(p.vel)) // a bad setting must not poison the chain
            settled = true;
        if (settled) {
            p.pos = target;
            p.vel = {};
        } else
            unsettled = true;

        changed = changed || p.pos != before.pos || p.vel != before.vel;
        target  = p.pos;
    }

    // Births follow activeMs: fresh while moving, aging once it stops.
    if (unsettled && m_activeMs != nowMs) {
        m_activeMs = nowMs;
        changed    = true;
    }
    m_unsettled = unsettled;

    if (changed)
        rebuild();
}

void CSpringChainSource::configure(const std::map<std::string, double>& values) {
    const auto get = [&](const char* name, float current) {
        const auto it = values.find(name);
        return it == values.end() ? current : static_cast<float>(it->second);
    };
    m_curve.mass      = get("mass", m_curve.mass);
    m_curve.stiffness = get("stiffness", m_curve.stiffness);
    m_curve.damping   = get("damping", m_curve.damping);

    if (const float step = get("age_step_ms", m_ageStepMs); step != m_ageStepMs) {
        m_ageStepMs = step;
        if (m_seeded)
            rebuild();
    }
}

void CSpringChainSource::resize(size_t capacity) {
    capacity = std::max<size_t>(capacity, 1);
    if (capacity == m_capacity)
        return;
    m_capacity = capacity;

    if (!m_seeded) {
        ++m_generation;
        return;
    }

    // Keeps the head end. New points start on the old tail, at rest.
    SPoint tail = m_points.back();
    tail.vel    = {};
    m_points.resize(capacity, tail);
    rebuild();
}

void CSpringChainSource::clear() {
    m_seeded    = false;
    m_unsettled = false;
    m_points.clear();
    m_nodes.clear();
    ++m_generation;
}

size_t CSpringChainSource::size() const {
    return m_points.size();
}

size_t CSpringChainSource::capacity() const {
    return m_capacity;
}

bool CSpringChainSource::empty() const {
    return !m_seeded;
}

const SCursorNode& CSpringChainSource::newest() const {
    return m_last;
}

uint64_t CSpringChainSource::generation() const {
    return m_generation;
}

double CSpringChainSource::newestBirthMs() const {
    return m_nodes.front().birthTimeMs;
}

void CSpringChainSource::orderedCopy(std::vector<SGpuNode>& out, double refMs) const {
    out.clear();
    out.reserve(m_nodes.size());
    for (size_t k = m_nodes.size(); k-- > 0;) { // tail first
        const auto& n = m_nodes[k];
        out.push_back(SGpuNode{
            .posPx    = n.posPx,
            .birthMs  = static_cast<float>(n.birthTimeMs - refMs),
            .velocity = n.velocity,
            .distPx   = static_cast<float>(n.distPx),
            .bits     = (n.seed << 1) | (n.segmentStart ? GPU_BIT_SEGMENT_START : 0u),
        });
    }
}

bool CSpringChainSource::needsContinuousUpload() const {
    return m_unsettled;
}

size_t CSpringChainSource::visibleCount(double nowMs, double fadeMs) const {
    return visibleCountOf(m_nodes.size(), [this](size_t i) -> const SCursorNode& { return m_nodes[i]; }, nowMs, fadeMs);
}

std::optional<STrailBounds> CSpringChainSource::visibleBounds(double nowMs, double fadeMs, bool includeOlderNode) const {
    return visibleBoundsOf(m_nodes.size(), [this](size_t i) -> const SCursorNode& { return m_nodes[i]; }, nowMs, fadeMs, includeOlderNode);
}

bool CSpringChainSource::isSettled(double nowMs, double fadeMs) const {
    return !m_unsettled && visibleCount(nowMs, fadeMs) == 0;
}

void CSpringChainSource::rebuild() {
    const size_t n = m_points.size();
    m_nodes.resize(n);

    double dist = 0.0; // from the tail, where the one segment starts
    for (size_t k = n; k-- > 0;) {
        const auto& p = m_points[k];
        if (k + 1 < n)
            dist += std::hypot(p.pos.x - m_points[k + 1].pos.x, p.pos.y - m_points[k + 1].pos.y);
        m_nodes[k] = SCursorNode{
            .posPx        = p.pos,
            .birthTimeMs  = m_activeMs - static_cast<double>(k) * m_ageStepMs,
            .velocity     = {p.vel.x / 1000.F, p.vel.y / 1000.F}, // px/s -> px/ms
            .distPx       = dist,
            .seed         = nodeSeed(m_seedBase, k),
            .segmentStart = k + 1 == n,
        };
    }
    ++m_generation;
}
