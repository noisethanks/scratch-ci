#include "TrailBuffer.hpp"

#include <algorithm>

CTrailRing::CTrailRing(size_t capacity) : m_nodes(std::max<size_t>(capacity, 1)) {}

void CTrailRing::insert(const SVec2f& pos, double nowMs, bool segmentStart) {
    SVec2f velocity{};
    if (m_count > 0 && !segmentStart) {
        const auto&  prev = newest();
        const double dt   = nowMs - prev.birthTimeMs;
        if (dt > 0.0)
            velocity = {static_cast<float>((pos.x - prev.posPx.x) / dt), static_cast<float>((pos.y - prev.posPx.y) / dt)};
    }

    m_nodes[m_head] = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity, .segmentStart = segmentStart || m_count == 0};
    m_head          = (m_head + 1) % m_nodes.size();
    m_count         = std::min(m_count + 1, m_nodes.size());
    ++m_generation;
}

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
            .flags    = n.segmentStart ? GPU_FLAG_SEGMENT_START : 0.F,
        });
    }
}

std::optional<STrailBounds> CTrailRing::visibleBounds(double nowMs, double fadeMs) const {
    std::optional<STrailBounds> b;

    const auto                  add = [&b](const SVec2f& p) {
        if (!b)
            b = STrailBounds{p.x, p.y, p.x, p.y};
        else {
            b->x1 = std::min(b->x1, p.x);
            b->y1 = std::min(b->y1, p.y);
            b->x2 = std::max(b->x2, p.x);
            b->y2 = std::max(b->y2, p.y);
        }
    };

    const size_t cap = m_nodes.size();
    for (size_t i = 0; i < m_count; ++i) {
        const auto& n = m_nodes[(m_head + cap - 1 - i) % cap]; // newest first
        if (nowMs - n.birthTimeMs >= fadeMs) {
            // First faded node. It still bounds the segment to the oldest
            // visible node, unless that one starts a new segment.
            const auto& newer = m_nodes[(m_head + cap - i) % cap];
            if (b && !newer.segmentStart)
                add(n.posPx);
            break;
        }

        add(n.posPx);
    }

    return b;
}
