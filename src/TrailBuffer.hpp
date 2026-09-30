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

// Whatever produces the trail's point buffer (SPEC §13.1). Not a general
// animation interface: it covers feeding points in, advancing the source,
// and handing the GPU upload an ordered copy of them.
class ISource {
  public:
    virtual ~ISource() = default;

    // segmentStart: don't connect to the previous node (no segment drawn
    // between them, no velocity or distance across the gap).
    virtual void   insert(const SVec2f& pos, double nowMs, bool segmentStart) = 0;

    // Advance the source to nowMs, dt ms after the previous tick. Sources
    // whose points only change on insert do nothing.
    virtual void   tick(double nowMs, double dt) = 0;

    // Oldest -> newest, rebuilt from scratch into out, converting to the GPU
    // layout with birth times relative to refMs.
    virtual void   orderedCopy(std::vector<SGpuNode>& out, double refMs) const = 0;

    // True if the points change between inserts, so the upload can't wait
    // for a new generation and happens every frame.
    virtual bool   needsContinuousUpload() const = 0;

    // Nothing left that can draw at nowMs for a layer fading over fadeMs.
    virtual bool   isSettled(double nowMs, double fadeMs) const = 0;

    // What the upload reads around orderedCopy().
    virtual uint64_t generation() const       = 0; // bumped whenever orderedCopy() would differ; gates uploads
    virtual bool     empty() const            = 0;
    virtual double   newestBirthMs() const    = 0; // requires !empty(); the upload's reference time
};

// Whether the node buffer must upload this frame: something to upload, and
// either the source changed since uploadedGen or it changes every frame.
inline bool sourceNeedsUpload(const ISource& s, uint64_t uploadedGen) {
    return !s.empty() && (s.generation() != uploadedGen || s.needsContinuousUpload());
}

// Fixed-capacity circular buffer of real pointer history, the single source
// of truth for one trail instance and the first ISource. Writes are O(1) at
// the head, the storage is never shifted.
class CTrailRing final : public ISource {
  public:
    // seedBase: per-load random value; node seeds hash it with an insertion
    // counter.
    CTrailRing(size_t capacity, uint64_t seedBase);

    void               insert(const SVec2f& pos, double nowMs, bool segmentStart) override;

    // Points only change on insert, so there is nothing to advance.
    void               tick(double nowMs, double dt) override;

    // Never: an upload is only needed when the generation changed.
    bool               needsContinuousUpload() const override;

    // visibleCount(nowMs, fadeMs) == 0: nothing is drawn, the same test the
    // draw and damage paths use.
    bool               isSettled(double nowMs, double fadeMs) const override;

    double             newestBirthMs() const override;

    // New capacity, keeping the newest min(size(), capacity) nodes in order.
    // Bumps the generation.
    void               resize(size_t capacity);

    // Drop every node (capacity unchanged). Bumps the generation.
    void               clear();

    size_t             size() const;
    size_t             capacity() const;
    bool               empty() const override;
    const SCursorNode& newest() const; // requires !empty()

    // Bumped on every insert, resize and clear. Gates VBO uploads.
    uint64_t generation() const override;

    // Stateless projection of the ring, not a second source of truth.
    void orderedCopy(std::vector<SGpuNode>& out, double refMs) const override;

    // Nodes that can still draw at nowMs: those with age < fadeMs. Birth
    // times are monotonic in insertion order, so they are always the newest
    // ones (a suffix of the ring): walks newest -> oldest and stops at the
    // first faded node.
    size_t visibleCount(double nowMs, double fadeMs) const;

    // Extent of everything that can still draw at nowMs: the visibleCount()
    // newest nodes, plus (includeOlderNode) the next older node if it's
    // connected to the oldest visible one: a path layer still draws that
    // segment, fading toward the older end. An instanced layer draws
    // visible nodes only, so it passes false. nullopt if nothing is visible.
    std::optional<STrailBounds> visibleBounds(double nowMs, double fadeMs, bool includeOlderNode = true) const;

  private:
    std::vector<SCursorNode> m_nodes;
    size_t                   m_head       = 0; // next write index
    size_t                   m_count      = 0;
    uint64_t                 m_generation = 0;
    uint64_t                 m_seedBase   = 0;
    uint64_t                 m_inserted   = 0;
};
