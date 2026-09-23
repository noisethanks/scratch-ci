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
    SVec2f posPx;        // global layout (logical) pixels, not per-monitor
    double birthTimeMs;  // ms since plugin load
    SVec2f velocity;     // px/ms, relative to the previous node; zero for a segment start
    bool   segmentStart; // not connected to the previous node (SPEC §7)
};

// SGpuNode::flags bits.
constexpr float GPU_FLAG_SEGMENT_START = 1.F; // bit 0

// One trail point as uploaded to the VBO. Field order and offsets are the
// vertex attribute contract (see CTrailGpu). birthMs is relative to a
// reference time chosen at upload, so it stays small and precise as float;
// the shader gets nowMs relative to the same reference, only the difference
// (age = nowMs - birthMs) is meaningful.
struct SGpuNode {
    SVec2f posPx;
    float  birthMs;
    SVec2f velocity;
    float  flags; // bit field stored as float (GLSL ES reads it with mod), see GPU_FLAG_*
};

static_assert(offsetof(SGpuNode, posPx) == 0);
static_assert(offsetof(SGpuNode, birthMs) == 8);
static_assert(offsetof(SGpuNode, velocity) == 12);
static_assert(offsetof(SGpuNode, flags) == 20);
static_assert(sizeof(SGpuNode) == 24);

// Axis-aligned extent of node positions, global layout pixels.
struct STrailBounds {
    float x1 = 0.F, y1 = 0.F, x2 = 0.F, y2 = 0.F;
};

// Fixed-capacity circular buffer, the single source of truth for one trail
// instance. Writes are O(1) at the head, the storage is never shifted.
class CTrailRing {
  public:
    explicit CTrailRing(size_t capacity);

    // segmentStart: don't connect to the previous node (no segment drawn
    // between them, no velocity across the gap).
    void               insert(const SVec2f& pos, double nowMs, bool segmentStart);

    // New capacity, keeping the newest min(size(), capacity) nodes in order.
    // Bumps the generation.
    void               resize(size_t capacity);

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
};
