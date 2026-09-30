#include "TrailBuffer.hpp"

#include <algorithm>
#include <cmath>

CTrailRing::CTrailRing(size_t capacity, uint64_t seedBase) : m_nodes(std::max<size_t>(capacity, 1)), m_seedBase(seedBase) {}

std::string_view CTrailRing::kind() const {
    return "pointer";
}

void CTrailRing::insert(const SVec2f& pos, double nowMs, bool segmentStart) {
    segmentStart = segmentStart || m_count == 0;

    SVec2f velocity{};
    double dist = 0.0; // restarts at every segment start
    if (!segmentStart) {
        const auto&  prev = newest();
        const double dt   = nowMs - prev.birthTimeMs;
        if (dt > 0.0)
            velocity = {static_cast<float>((pos.x - prev.posPx.x) / dt), static_cast<float>((pos.y - prev.posPx.y) / dt)};
        dist = prev.distPx + std::hypot(pos.x - prev.posPx.x, pos.y - prev.posPx.y);
    }

    const auto seed = nodeSeed(m_seedBase, m_inserted++); // 31 bits

    m_nodes[m_head] = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity, .distPx = dist, .seed = seed, .segmentStart = segmentStart};
    m_head          = (m_head + 1) % m_nodes.size();
    m_count         = std::min(m_count + 1, m_nodes.size());
    ++m_generation;
}

void CTrailRing::tick(double, double) {}

void CTrailRing::configure(const std::map<std::string, double>&) {}

void CTrailRing::resize(size_t capacity) {
    capacity = std::max<size_t>(capacity, 1);
    if (capacity == m_nodes.size())
        return;

    const size_t             keep   = std::min(m_count, capacity);
    const size_t             cap    = m_nodes.size();
    const size_t             oldest = (m_head + cap - keep) % cap; // oldest of the kept ones
    std::vector<SCursorNode> nodes(capacity);
    for (size_t i = 0; i < keep; ++i)
        nodes[i] = m_nodes[(oldest + i) % cap];

    m_nodes = std::move(nodes);
    m_count = keep;
    m_head  = keep % capacity;
    ++m_generation;
}

void CTrailRing::clear() {
    m_head  = 0;
    m_count = 0;
    ++m_generation;
}

size_t CTrailRing::size() const {
    return m_count;
}

size_t CTrailRing::capacity() const {
    return m_nodes.size();
}

bool CTrailRing::empty() const {
    return m_count == 0;
}

const SCursorNode& CTrailRing::newest() const {
    return m_nodes[(m_head + m_nodes.size() - 1) % m_nodes.size()];
}

uint64_t CTrailRing::generation() const {
    return m_generation;
}

double CTrailRing::newestBirthMs() const {
    return newest().birthTimeMs;
}

bool CTrailRing::needsContinuousUpload() const {
    return false;
}

bool CTrailRing::isSettled(double nowMs, double fadeMs) const {
    return visibleCount(nowMs, fadeMs) == 0;
}

void CTrailRing::orderedCopy(std::vector<SGpuNode>& out, double refMs) const {
    out.clear();
    out.reserve(m_count);

    const size_t cap    = m_nodes.size();
    const size_t oldest = (m_head + cap - m_count) % cap;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& n = m_nodes[(oldest + i) % cap];
        out.push_back(SGpuNode{
            .posPx    = n.posPx,
            .birthMs  = static_cast<float>(n.birthTimeMs - refMs),
            .velocity = n.velocity,
            .distPx   = static_cast<float>(n.distPx),
            .bits     = (n.seed << 1) | (n.segmentStart ? GPU_BIT_SEGMENT_START : 0u),
        });
    }
}

const SCursorNode& CTrailRing::at(size_t i) const {
    const size_t cap = m_nodes.size();
    return m_nodes[(m_head + cap - 1 - i) % cap];
}

size_t CTrailRing::visibleCount(double nowMs, double fadeMs) const {
    return visibleCountOf(m_count, [this](size_t i) -> const SCursorNode& { return at(i); }, nowMs, fadeMs);
}

std::optional<STrailBounds> CTrailRing::visibleBounds(double nowMs, double fadeMs, bool includeOlderNode) const {
    return visibleBoundsOf(m_count, [this](size_t i) -> const SCursorNode& { return at(i); }, nowMs, fadeMs, includeOlderNode);
}
