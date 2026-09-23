#pragma once

#include <cstdint>
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
// Attribute layout (both sets read the same VBO, divisor 1):
//   locations 0..2  node i     (offset 0)          pos, birthMs, velocity
//   locations 3..5  node i + 1 (offset one stride) pos, birthMs, velocity
// So instance i sees the segment node[i] -> node[i+1], and gl_VertexID picks
// the quad corner. The dot harness draws size() instances and reads only set
// 0; the ribbon (SPEC §4) will draw size() - 1 instances and use both.
// The VBO holds one extra trailing node (a copy of the newest) so set 1 never
// reads past the uploaded data, for any instance count up to size().
class CTrailGpu {
  public:
    // No GL in the destructor: destroy() runs explicitly while the context is
    // current (PLUGIN_EXIT).
    bool   ensure(size_t ringCapacity); // creates VAO/VBO on first call
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
    bool        initFailed  = false;
    GLint       locRadius   = -1;
    GLint       locSpeedRef = -1;
    GLint       locNowMs    = -1;
    GLint       locFadeMs   = -1;
};

struct SMonitorTrailState {
    CBox prevBoxLocal{}; // logical, monitor-local, box damaged last frame; empty once cleared
};

// One trail: buffer, GPU mirror, shader, per-monitor damage state. Written
// per instance so the idle/presence slot (SPEC §7) can be a second one.
struct STrailInstance {
    STrailInstance(size_t capacity, double fadeMs_) : ring(capacity), fadeMs(fadeMs_) {}

    CTrailRing                                                 ring;
    CTrailGpu                                                  gpu;
    STrailShader                                               shader;

    double                                                     fadeMs;
    float                                                      radiusPx        = 6.F; // logical, also the damage padding
    float                                                      speedRefPxPerMs = 2.F; // speed mapped to full red

    std::unordered_map<Monitor::CMonitor*, SMonitorTrailState> monState;
};

// Bounds padded by radiusPx, logical, monitor-local.
CBox trailBoxLocal(const STrailInstance& inst, const STrailBounds& bounds, const Vector2D& monitorPos);

// Dot harness: faded dots from the instance's VBO.
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
    STrailInstance* m_inst = nullptr;
    CBox            m_boxLocal;
    double          m_nowMs = 0.0;
};

// Call from PLUGIN_EXIT, after removing queued elements, GL context current.
void trailInstanceCleanup(STrailInstance& inst);
