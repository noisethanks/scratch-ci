#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <render/pass/PassElement.hpp>
#include <render/Shader.hpp>
#include <helpers/memory/Memory.hpp>

#include "TrailBuffer.hpp"

namespace Monitor {
    class CMonitor;
}

// GPU mirror of one CTrailRing: its own VAO + VBO of SGpuNode.
//
// VBO: [front pad, n0 .. n(count-1), back pad]. The front pad is a copy of n0
// flagged as a segment start, the back pad a copy of the newest node, so the
// ends have a zero-length neighbor ("no neighbor" in the shader).
//
// Four bindings of the same VBO, divisor 1, at consecutive node offsets, so
// instance i sees prev = n(i-1), p0 = n(i), p1 = n(i+1), next = n(i+2):
//   locations 0..1   prev  (VBO offset 0 strides)  pos, flags
//   locations 2..5   p0    (1 stride)              pos, birthMs, velocity, flags
//   locations 6..9   p1    (2 strides)             pos, birthMs, velocity, flags
//   locations 10..11 next  (3 strides)             pos, flags
// The ribbon draws size() - 1 instances, one per segment p0 -> p1; the
// shader contract is documented in shaders/trail.vert.
class CTrailGpu {
  public:
    // No GL in the destructor: destroy() runs explicitly while the context is
    // current (PLUGIN_EXIT).
    // Creates VAO/VBO on first call. On failure returns false with a
    // description in error, and leaves nothing allocated.
    bool   ensure(size_t ringCapacity, std::string& error);
    void   upload(const CTrailRing& ring);
    void   destroy();

    GLuint vao() const;
    double refMs() const; // reference time of the uploaded birthMs values

  private:
    GLuint                m_vao         = 0;
    GLuint                m_vbo         = 0;
    size_t                m_vboNodes    = 0;
    uint64_t              m_uploadedGen = UINT64_MAX;
    double                m_refMs       = 0.0;
    std::vector<SGpuNode> m_ordered;
};

struct STrailShader {
    SP<CShader> shader;
    GLint       locNowMs      = -1;
    GLint       locFadeMs     = -1;
    GLint       locWidthPx    = -1;
    GLint       locMiterLimit = -1;
    GLint       locSpeedRef   = -1;
};

struct SMonitorTrailState {
    CBox prevBoxLocal{}; // logical, monitor-local, box damaged last frame; empty once cleared
};

// One trail: buffer, GPU mirror, shader, per-monitor damage state. Written
// per instance so the idle/presence slot (SPEC §7) can be a second one.
struct STrailInstance {
    STrailInstance(std::string name_, size_t capacity, double fadeMs_) : name(std::move(name_)), ring(capacity), fadeMs(fadeMs_) {}

    std::string                                                name; // for diagnostics keys, e.g. "shader:<name>"
    CTrailRing                                                 ring;
    CTrailGpu                                                  gpu;
    STrailShader                                               shader;

    double                                                     fadeMs;

    // Placeholders (SPEC §4/§5).
    float widthPx         = 8.F; // full ribbon width at age 0, logical px, tapers with age
    float miterLimit      = 2.F; // max miter length in half-widths
    float speedRefPxPerMs = 2.F; // speed mapped to the fast end of the palette
    float minSpacingPx    = 2.F; // min distance between inserted nodes, limits inner-miter folding
    bool  interpolateWarps = false; // warps via CPointerController connect (true) or start a new segment (false)

    // Damage padding around the node extent: widest possible miter plus the
    // ~1px antialiased edge. Must cover everything the shader can draw.
    float padPx() const {
        return 0.5F * widthPx * miterLimit + 1.F;
    }

    // Next insert starts a new segment (not connected to the previous node):
    // set on workspace changes, lock, pointer constraints, and warps unless
    // interpolateWarps.
    bool pendingBreak = false;

    std::unordered_map<Monitor::CMonitor*, SMonitorTrailState> monState;

    // Set after an unrecoverable failure (shader, GL resources, exception).
    // The lifecycle then treats the trail as fully faded: no inserts, the last
    // box is damaged once to clear it, no element, idle. Stays set until the
    // plugin is reloaded.
    bool disabled = false;
};

// Reports ERR under key and disables the instance. Safe from any context; GPU
// resources are released separately (trailReleaseGpu) where GL is current.
void trailDisable(STrailInstance& inst, std::string_view key, std::string_view message);

// GL context must be current (inside a render, or PLUGIN_EXIT).
void trailReleaseGpu(STrailInstance& inst);

// Bounds padded by padPx(), logical, monitor-local.
CBox trailBoxLocal(const STrailInstance& inst, const STrailBounds& bounds, const Vector2D& monitorPos);

// Trail ribbon from the instance's VBO, one instance per segment.
class CTrailPassElement : public IPassElement {
  public:
    // nowMs is the same instant the caller used for visibility/damage, so
    // anything the shader draws with alpha > 0 is inside the damaged box.
    CTrailPassElement(STrailInstance* inst, const CBox& boxLocal, double nowMs);
    ~CTrailPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;

    bool                          needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }

    std::optional<CBox> boundingBox() override;

    const char*         passName() override {
        return "CTrailPassElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    void            drawInternal();

    STrailInstance* m_inst = nullptr;
    CBox            m_boxLocal;
    double          m_nowMs = 0.0;
};

// Call from PLUGIN_EXIT, after removing queued elements, GL context current.
// Releases GPU resources and per-monitor state.
void trailInstanceCleanup(STrailInstance& inst);
