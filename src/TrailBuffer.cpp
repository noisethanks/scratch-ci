#include "TrailBuffer.hpp"

#include <algorithm>
#include <cmath>

namespace {
    // splitmix64 finalizer: well-mixed bits from a counter.
    uint64_t mix64(uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }
}

CTrailRing::CTrailRing(size_t capacity, uint64_t seedBase) : m_nodes(std::max<size_t>(capacity, 1)), m_seedBase(seedBase) {}

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

    const auto seed = static_cast<uint32_t>(mix64(m_seedBase + m_inserted++) >> 33); // 31 bits

    m_nodes[m_head] = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity, .distPx = dist, .seed = seed, .segmentStart = segmentStart};
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

size_t CTrailRing::visibleCount(double nowMs, double fadeMs) const {
    const size_t cap = m_nodes.size();
    size_t       n   = 0;
    while (n < m_count && nowMs - m_nodes[(m_head + cap - 1 - n) % cap].birthTimeMs < fadeMs)
        ++n;
    return n;
}

std::optional<STrailBounds> CTrailRing::visibleBounds(double nowMs, double fadeMs, bool includeOlderNode) const {
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
            if (includeOlderNode && b && !newer.segmentStart)
                add(n.posPx);
            break;
        }

        add(n.posPx);
    }

    return b;
}
