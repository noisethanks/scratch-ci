#include "TrailBuffer.hpp"

#include <algorithm>

CTrailRing::CTrailRing(size_t capacity) : m_nodes(std::max<size_t>(capacity, 1)) {}

void CTrailRing::insert(const SVec2f& pos, float nowMs) {
    SVec2f velocity{};
    if (m_count > 0) {
        const auto& prev = newest();
        const float dt   = nowMs - prev.birthTimeMs;
        if (dt > 0.F)
            velocity = {(pos.x - prev.posPx.x) / dt, (pos.y - prev.posPx.y) / dt};
    }

    m_nodes[m_head] = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity};
    m_head          = (m_head + 1) % m_nodes.size();
    m_count         = std::min(m_count + 1, m_nodes.size());
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

void CTrailRing::orderedCopy(std::vector<SCursorNode>& out) const {
    out.clear();
    out.reserve(m_count);

    const size_t cap    = m_nodes.size();
    const size_t oldest = (m_head + cap - m_count) % cap;
    for (size_t i = 0; i < m_count; ++i)
        out.push_back(m_nodes[(oldest + i) % cap]);
}

STrailBounds CTrailRing::bounds() const {
    const auto&  first = newest();
    STrailBounds b{first.posPx.x, first.posPx.y, first.posPx.x, first.posPx.y};

    // Order doesn't matter for an extent, walk the valid slots directly.
    const size_t cap    = m_nodes.size();
    const size_t oldest = (m_head + cap - m_count) % cap;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& p = m_nodes[(oldest + i) % cap].posPx;
        b.x1          = std::min(b.x1, p.x);
        b.y1          = std::min(b.y1, p.y);
        b.x2          = std::max(b.x2, p.x);
        b.y2          = std::max(b.y2, p.y);
    }

    return b;
}
