#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
// vertex attribute layout (CNodeBuffer, hyprtail/shaders/prelude/path.glsl). birthMs
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

// splitmix64 finalizer: well-mixed bits from a counter.
inline uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// A node's 31-bit seed from a per-load base and a counter.
inline uint32_t nodeSeed(uint64_t base, uint64_t counter) {
    return static_cast<uint32_t>(mix64(base + counter) >> 33);
}

// Visibility over a node sequence, shared by the sources. `at(i)` is the
// i-th newest node, i in [0, count); birth times never increase with i, so
// the visible nodes (age < fadeMs) are always the newest ones, a prefix.
template <class At>
size_t visibleCountOf(size_t count, const At& at, double nowMs, double fadeMs) {
    size_t n = 0;
    while (n < count && nowMs - at(n).birthTimeMs < fadeMs)
        ++n;
    return n;
}

template <class At>
std::optional<STrailBounds> visibleBoundsOf(size_t count, const At& at, double nowMs, double fadeMs, bool includeOlderNode) {
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

    for (size_t i = 0; i < count; ++i) {
        const auto& n = at(i);
        if (nowMs - n.birthTimeMs >= fadeMs) {
            // First faded node. It still bounds the segment to the oldest
            // visible node, unless that one starts a new segment. (b is set
            // only after a visible node, so i >= 1 here.)
            if (includeOlderNode && b && !at(i - 1).segmentStart)
                add(n.posPx);
            break;
        }

        add(n.posPx);
    }

    return b;
}

// What produces the trail's point buffer (SPEC §13.1): feeding points in,
// advancing them in time, and handing the GPU upload an ordered copy. Not a
// general animation interface.
class ISource {
  public:
    virtual ~ISource() = default;

    // "pointer" or "spring": the `source` key of a preset (SPEC §13.7).
    virtual std::string_view kind() const = 0;

    // The pointer is at pos. segmentStart: don't connect to what came before
    // (a pointer history starts a new segment; a spring chain is re-seeded at
    // pos, there is no severed piece left to fade).
    virtual void insert(const SVec2f& pos, double nowMs, bool segmentStart) = 0;

    // Advance the source to nowMs, dt ms after the previous tick (0 = no
    // time passed). Sources whose points only change on insert do nothing.
    virtual void tick(double nowMs, double dt) = 0;

    // Source settings by name (SPEC §13.7 `source:<name>`), applied live.
    // Names the source doesn't know are ignored; the caller validated them.
    virtual void configure(const std::map<std::string, double>& values) = 0;

    // New capacity, keeping what fits. Bumps the generation if it changed.
    virtual void resize(size_t capacity) = 0;

    // Drop every node (capacity unchanged). Bumps the generation.
    virtual void   clear() = 0;

    virtual size_t size() const     = 0;
    virtual size_t capacity() const = 0;
    virtual bool   empty() const    = 0;

    // The most recent insert: its position (the spacing gate and the warp
    // curve start from it), time and velocity. Requires !empty().
    virtual const SCursorNode& newest() const = 0;

    // Bumped whenever orderedCopy() would return something different.
    virtual uint64_t generation() const = 0;

    // The upload's reference time: the birth time of the newest node.
    // Requires !empty().
    virtual double newestBirthMs() const = 0;

    // Oldest -> newest, rebuilt from scratch into out, converting to the GPU
    // layout with birth times relative to refMs. Stateless projection of the
    // source, not a second source of truth.
    virtual void orderedCopy(std::vector<SGpuNode>& out, double refMs) const = 0;

    // True while the points change between inserts (every tick moves them),
    // so the upload can't wait for a new generation.
    virtual bool needsContinuousUpload() const = 0;

    // Nodes that can still draw at nowMs: those with age < fadeMs, always
    // the newest ones.
    virtual size_t visibleCount(double nowMs, double fadeMs) const = 0;

    // Extent of everything that can still draw at nowMs: the visibleCount()
    // newest nodes, plus (includeOlderNode) the next older node if it's
    // connected to the oldest visible one: a path layer still draws that
    // segment, fading toward the older end. An instanced layer draws
    // visible nodes only, so it passes false. nullopt if nothing is visible.
    virtual std::optional<STrailBounds> visibleBounds(double nowMs, double fadeMs, bool includeOlderNode = true) const = 0;

    // Nothing left to draw at nowMs for a layer fading over fadeMs: the
    // source has stopped moving and every node has faded. Neither alone is
    // enough; a source that stopped but hasn't faded is not settled.
    virtual bool isSettled(double nowMs, double fadeMs) const = 0;
};

// Whether the node buffer must upload this frame: something to upload, and
// either the source changed since uploadedGen or it changes every frame.
inline bool sourceNeedsUpload(const ISource& s, uint64_t uploadedGen) {
    return !s.empty() && (s.generation() != uploadedGen || s.needsContinuousUpload());
}

// The per-render sample's spacing gate (main.cpp's sampleSource): a sample
// at pos inserts a node into an empty source, else only at least
// minSpacingPx from the newest node.
inline bool sampleInserts(const ISource& s, const SVec2f& pos, float minSpacingPx) {
    if (s.empty())
        return true;
    const auto& newest = s.newest().posPx;
    return std::hypot(pos.x - newest.x, pos.y - newest.y) >= minSpacingPx;
}

// Fixed-capacity circular buffer of real pointer history, the single source
// of truth for one trail instance and the `pointer` ISource. Writes are O(1)
// at the head, the storage is never shifted.
class CTrailRing final : public ISource {
  public:
    // seedBase: per-load random value; node seeds hash it with an insertion
    // counter.
    CTrailRing(size_t capacity, uint64_t seedBase);

    std::string_view kind() const override;

    // segmentStart: don't connect to the previous node (no segment drawn
    // between them, no velocity or distance across the gap).
    void insert(const SVec2f& pos, double nowMs, bool segmentStart) override;

    // Points only change on insert, so there is nothing to advance.
    void tick(double nowMs, double dt) override;

    // The ring has no settings.
    void configure(const std::map<std::string, double>& values) override;

    // New capacity, keeping the newest min(size(), capacity) nodes in order.
    // Bumps the generation.
    void resize(size_t capacity) override;

    // Bumps the generation.
    void               clear() override;

    size_t             size() const override;
    size_t             capacity() const override;
    bool               empty() const override;
    const SCursorNode& newest() const override; // the newest node

    // Bumped on every insert, resize and clear. Gates VBO uploads.
    uint64_t generation() const override;

    double   newestBirthMs() const override;

    void     orderedCopy(std::vector<SGpuNode>& out, double refMs) const override;

    // Never: an upload is only needed when the generation changed.
    bool needsContinuousUpload() const override;

    // Birth times are monotonic in insertion order, so the visible nodes are
    // a suffix of the ring: walks newest -> oldest and stops at the first
    // faded node.
    size_t                      visibleCount(double nowMs, double fadeMs) const override;

    std::optional<STrailBounds> visibleBounds(double nowMs, double fadeMs, bool includeOlderNode = true) const override;

    // visibleCount(nowMs, fadeMs) == 0: the points never move, so fading is
    // all there is to wait for.
    bool isSettled(double nowMs, double fadeMs) const override;

  private:
    // i-th newest node.
    const SCursorNode&       at(size_t i) const;

    std::vector<SCursorNode> m_nodes;
    size_t                   m_head       = 0; // next write index
    size_t                   m_count      = 0;
    uint64_t                 m_generation = 0;
    uint64_t                 m_seedBase   = 0;
    uint64_t                 m_inserted   = 0;
};
