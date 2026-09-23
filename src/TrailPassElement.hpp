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

// GPU mirror of one CTrailRing: its own VAO + VBO.
//
// Attribute layout (both sets read the same VBO, divisor 1):
//   locations 0..2  node i     (offset 0)          pos, birthTimeMs, velocity
//   locations 3..5  node i + 1 (offset one stride) pos, birthTimeMs, velocity
// So instance i sees the segment node[i] -> node[i+1], and gl_VertexID picks
// the quad corner. The stage-3 dot harness draws size() instances and reads
// only set 0; the ribbon (SPEC §4) will draw size() - 1 instances and use both.
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

  private:
    GLuint                   m_vao         = 0;
    GLuint                   m_vbo         = 0;
    size_t                   m_vboNodes    = 0;
    uint64_t                 m_uploadedGen = UINT64_MAX;
    std::vector<SCursorNode> m_ordered;
};

struct STrailShader {
    SP<CShader> shader;
    bool        initFailed = false;
    GLint       locRadiusMin = -1;
    GLint       locRadiusMax = -1;
    GLint       locSpeedRef  = -1;
    GLint       locNodeCount = -1;
};

struct SMonitorTrailState {
    CBox     prevBoxLocal{}; // logical, monitor-local, last box damaged
    uint64_t seenGeneration = 0;
};

// One trail: buffer, GPU mirror, shader, per-monitor damage state. Written
// per instance so the idle/presence slot (SPEC §7) can be a second one.
struct STrailInstance {
    explicit STrailInstance(size_t capacity) : ring(capacity) {}

    CTrailRing                                              ring;
    CTrailGpu                                               gpu;
    STrailShader                                            shader;

    float                                                   radiusMinPx = 2.F; // logical, oldest dot
    float                                                   radiusMaxPx = 6.F; // logical, newest dot, also damage extent
    float                                                   speedRefPxPerMs = 2.F; // speed mapped to full red

    std::unordered_map<Monitor::CMonitor*, SMonitorTrailState> monState;
};

// Trail extent (node bounds padded by radiusMaxPx), logical, monitor-local.
// Requires !inst.ring.empty().
CBox trailBoxLocal(const STrailInstance& inst, const Vector2D& monitorPos);

// Stage-3 harness: row of dots from the instance's VBO.
class CTrailPassElement : public IPassElement {
  public:
    CTrailPassElement(STrailInstance* inst, const CBox& boxLocal);
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
};

// Call from PLUGIN_EXIT, after removing queued elements, GL context current.
void trailInstanceCleanup(STrailInstance& inst);
