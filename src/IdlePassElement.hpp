#pragma once

#include <optional>
#include <string>

#include <render/pass/PassElement.hpp>
#include <helpers/Color.hpp>

#include "ShaderSlot.hpp"
#include "RenderUtil.hpp"

// Idle/presence slot (SPEC section 7, design B): a square around the pointer,
// drawn by a user-replaceable fragment shader, shown after the pointer has
// been still for delayMs, for durationMs (0 = until it moves). No point
// buffer. Contract in shaders/idle.vert.
struct SIdleInstance {
    SIdleInstance() : slot(name, "idle.vert", hyprtail::shader::builtinIdleVertex(), "idle.frag", hyprtail::shader::builtinIdleFragment()) {}

    std::string              name = "idle";
    hyprtail::CShaderSlot    slot;
    hyprtail::CMonitorDamage damage;

    // Settings, from the config on load and every reload.
    bool       enabled         = false;
    double     delayMs         = 500.0;
    double     durationMs      = 1500.0; // 0 = until the pointer moves
    float      radiusPx        = 24.F;
    bool       whenHidden      = false; // draw around a hidden cursor
    float      damagePaddingPx = 0.F;
    CHyprColor colorSlow{0xFF1A66FFULL};
    CHyprColor colorFast{0xFFFF1A1AULL};

    // Pointer tracking: last position seen and when it last changed, in ms
    // since plugin load. Motion is noticed from pointer events, warps and
    // every render.
    Vector2D lastPos;
    double   lastMotionMs = 0.0;

    // Empty VAO: the idle shaders take no attributes (gl_VertexID only).
    GLuint vao = 0;

    // Set after an unrecoverable failure; the effect stays off until reload.
    bool disabled = false;

    // Half-size of the drawn square: radius plus shader-declared and
    // configured padding. The damage box adds 1px.
    float extentPx() const {
        return radiusPx + slot.declaredPaddingPx() + damagePaddingPx;
    }
};

// Time since the effect started if it should show at nowMs (pointer still
// for delayMs, duration not over), else nullopt. Doesn't check lock,
// constraint or cursor visibility; the caller does.
std::optional<double> idleEffectMs(const SIdleInstance& idle, double nowMs);

// Square around `center` (global) as a logical, monitor-local damage box.
CBox idleBoxLocal(const SIdleInstance& idle, const Vector2D& center, const Vector2D& monitorPos);

// Inside a render (GL current). False if the slot has no usable program
// (then the instance is disabled and reported).
bool idlePrepare(SIdleInstance& idle);

// GL context must be current.
void idleReleaseGpu(SIdleInstance& idle);

// PLUGIN_EXIT, after removing queued elements, GL current.
void idleCleanup(SIdleInstance& idle);

class CIdlePassElement : public IPassElement {
  public:
    CIdlePassElement(SIdleInstance* idle, const CBox& boxLocal, const Vector2D& center, double effectMs);
    ~CIdlePassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;

    bool                          needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }

    std::optional<CBox> boundingBox() override {
        return m_boxLocal;
    }

    const char* passName() override {
        return "CIdlePassElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    void           drawInternal();

    SIdleInstance* m_idle = nullptr;
    CBox           m_boxLocal;
    Vector2D       m_center;
    double         m_effectMs = 0.0;
};
