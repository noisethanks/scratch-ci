#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

struct SVec2f {
    float x = 0.F;
    float y = 0.F;

    bool  operator==(const SVec2f&) const = default;
};

// One trail point, CPU side (SPEC §3). Timestamps are double: a float of ms
// since plugin load stops resolving 1ms after ~4.6h (2^24 ms).
struct SCursorNode {
    SVec2f   posPx;        // global layout (logical) pixels, not per-monitor
    double   birthTimeMs;  // ms since plugin load
    SVec2f   velocity;     // px/ms, relative to the previous node; zero for a segment start
    double   distPx;       // path distance from the start of this node's segment
    uint32_t seed;         // 31 random bits, stable for the node's life
    bool     segmentStart; // not connected to the previous node (SPEC §7)
};

// SGpuNode::bits: bit 0 = segment start, bits 1..31 = seed.
constexpr uint32_t GPU_BIT_SEGMENT_START = 1u;

// One trail point as uploaded to the VBO. Field order and offsets are the
// vertex attribute layout (CNodeBuffer, shaders/prelude/path.glsl). birthMs
// is relative to a reference time chosen at upload, so it stays small and
// precise as float; the shader gets ht_nowMs relative to the same reference,
// only the difference (age) is meaningful. distPx is within the node's
// segment, which restarts at every break, so it stays small too.
struct SGpuNode {
    SVec2f   posPx;
    float    birthMs;
    SVec2f   velocity;
    float    distPx;
    uint32_t bits; // integer attribute, see GPU_BIT_*
};

static_assert(offsetof(SGpuNode, posPx) == 0);
static_assert(offsetof(SGpuNode, birthMs) == 8);
static_assert(offsetof(SGpuNode, velocity) == 12);
static_assert(offsetof(SGpuNode, distPx) == 20);
static_assert(offsetof(SGpuNode, bits) == 24);
static_assert(sizeof(SGpuNode) == 28);

// Axis-aligned extent of node positions, global layout pixels.
struct STrailBounds {
    float x1 = 0.F, y1 = 0.F, x2 = 0.F, y2 = 0.F;
};

// Fixed-capacity circular buffer, the single source of truth for one trail
// instance. Writes are O(1) at the head, the storage is never shifted.
class CTrailRing {
  public:
    // seedBase: per-load random value; node seeds hash it with an insertion
    // counter.
    CTrailRing(size_t capacity, uint64_t seedBase);

    // segmentStart: don't connect to the previous node (no segment drawn
    // between them, no velocity or distance across the gap).
    void               insert(const SVec2f& pos, double nowMs, bool segmentStart);

    // New capacity, keeping the newest min(size(), capacity) nodes in order.
    // Bumps the generation.
    void               resize(size_t capacity);

    // Drop every node (capacity unchanged). Bumps the generation.
    void               clear();

    size_t             size() const;
    size_t             capacity() const;
    bool               empty() const;
    const SCursorNode& newest() const; // requires !empty()

    // Bumped on every insert. Gates VBO uploads.
    uint64_t generation() const;

    // Oldest -> newest, rebuilt from scratch into out, converting to the GPU
    // layout with birth times relative to refMs. Stateless projection of the
    // ring, not a second source of truth.
    void orderedCopy(std::vector<SGpuNode>& out, double refMs) const;

    // Extent of everything that can still draw at nowMs: nodes with
    // age < fadeMs, plus the next older node if it's connected to the oldest
    // visible one (that segment still draws, fading toward the older end).
    // nullopt if nothing is visible. Birth times are monotonic in insertion
    // order, so the visible nodes are always the newest ones: walks
    // newest -> oldest and stops after the first faded node.
    std::optional<STrailBounds> visibleBounds(double nowMs, double fadeMs) const;

  private:
    std::vector<SCursorNode> m_nodes;
    size_t                   m_head       = 0; // next write index
    size_t                   m_count      = 0;
    uint64_t                 m_generation = 0;
    uint64_t                 m_seedBase   = 0;
    uint64_t                 m_inserted   = 0;
};
