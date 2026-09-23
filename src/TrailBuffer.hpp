#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct SVec2f {
    float x = 0.F;
    float y = 0.F;

    bool  operator==(const SVec2f&) const = default;
};

// One trail point (SPEC §3). The ordered copy of these is uploaded to the VBO
// as-is, so field order and offsets are part of the vertex attribute contract
// (see CTrailGpu). If the CPU and GPU layouts ever diverge (e.g. birthTimeMs
// precision, stage 4), CTrailRing::orderedCopy is where the conversion goes.
struct SCursorNode {
    SVec2f posPx;       // global layout (logical) pixels, not per-monitor
    float  birthTimeMs; // ms since plugin load
    SVec2f velocity;    // px/ms, relative to the previous node; zero for the first
};

static_assert(offsetof(SCursorNode, posPx) == 0);
static_assert(offsetof(SCursorNode, birthTimeMs) == 8);
static_assert(offsetof(SCursorNode, velocity) == 12);
static_assert(sizeof(SCursorNode) == 20);

// Axis-aligned extent of node positions, global layout pixels.
struct STrailBounds {
    float x1 = 0.F, y1 = 0.F, x2 = 0.F, y2 = 0.F;
};

// Fixed-capacity circular buffer, the single source of truth for one trail
// instance. Writes are O(1) at the head, the storage is never shifted.
class CTrailRing {
  public:
    explicit CTrailRing(size_t capacity);

    void               insert(const SVec2f& pos, float nowMs);

    size_t             size() const;
    size_t             capacity() const;
    bool               empty() const;
    const SCursorNode& newest() const; // requires !empty()

    // Bumped on every insert. Consumers (damage, upload) compare against the
    // generation they last saw instead of diffing contents.
    uint64_t     generation() const;

    // Oldest -> newest, rebuilt from scratch into out. Stateless projection of
    // the ring, not a second source of truth.
    void         orderedCopy(std::vector<SCursorNode>& out) const;

    STrailBounds bounds() const; // requires !empty()

  private:
    std::vector<SCursorNode> m_nodes;
    size_t                   m_head       = 0; // next write index
    size_t                   m_count      = 0;
    uint64_t                 m_generation = 0;
};
