#include "DotPassElement.hpp"

#include <render/Renderer.hpp>
#include <render/Shader.hpp>
#include <pointer/PointerManager.hpp>
#include <debug/log/Logger.hpp>
#include <hyprutils/math/Box.hpp>

using namespace Render;

// Sampled at element-construction time (inside the hook, before simplify/draw).
CDotPassElement::CDotPassElement() {
    const auto    pMonitor     = g_pHyprRenderer->m_renderData.pMonitor;
    const Vector2D cursorGlobal = Pointer::mgr()->position();
    const Vector2D localPos     = cursorGlobal - pMonitor->m_position;
    m_box = CBox{localPos.x - 10.0, localPos.y - 10.0, 20.0, 20.0};
}

std::optional<CBox> CDotPassElement::boundingBox() {
    return m_box;
}

static const std::string VERT_SRC = R"glsl(
#version 300 es
precision highp float;
uniform mat3 proj;
in vec2 pos;
void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
}
)glsl";

static const std::string FRAG_SRC = R"glsl(
#version 300 es
precision highp float;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)glsl";

static CShader* s_shader     = nullptr;
static bool     s_initFailed = false;

static void ensureShader() {
    if (s_shader || s_initFailed)
        return;

    s_shader = new CShader();
    if (!s_shader->createProgram(VERT_SRC, FRAG_SRC, /*dynamic=*/true, /*silent=*/false)) {
        LOG(Log::ERR, "[hyprtail-s2] shader compilation failed");
        delete s_shader;
        s_shader     = nullptr;
        s_initFailed = true;
        return;
    }

    LOG(Log::INFO, "[hyprtail-s2] shader compiled ok, program id={}", s_shader->program());
}

void dotPassCleanup() {
    if (s_shader) {
        s_shader->destroy();
        delete s_shader;
        s_shader = nullptr;
    }
    s_initFailed = false;
}

std::vector<UP<IPassElement>> CDotPassElement::draw() {
    ensureShader();
    if (!s_shader)
        return {};

    const auto glMatrix = g_pHyprRenderer->projectBoxToTarget(m_box);

    glUseProgram(s_shader->program());
    s_shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, glMatrix.getMatrix());

    glDisable(GL_SCISSOR_TEST);

    glBindVertexArray(s_shader->getUniformLocation(SHADER_SHADER_VAO));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glEnable(GL_SCISSOR_TEST);
    glUseProgram(0);

    return {};
}
