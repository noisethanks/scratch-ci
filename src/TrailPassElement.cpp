#include "TrailPassElement.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>

#include <render/Renderer.hpp>
#include <render/OpenGL.hpp>
#include <render/Shader.hpp>
#include <output/Monitor.hpp>
#include <debug/log/Logger.hpp>

#include "Diagnostics.hpp"

using namespace Render;
using hyprtail::diag::eSeverity;
using namespace Render::GL;

// ---------------------------------------------------------------- shader

// Stock shaders and prefabs are embedded and preprocessed by ShaderSource.
const SShaderPair& builtinShaderPair() {
    static const SShaderPair pair = [] {
        auto vert = hyprtail::shader::preprocess(hyprtail::shader::builtinVertex(), "trail.vert", {});
        auto frag = hyprtail::shader::preprocess(hyprtail::shader::builtinFragment(), "trail.frag", {});
        // Built-ins are fixed at build time; a failure here is a plugin bug.
        if (!vert || !frag)
            throw std::runtime_error(std::format("built-in shader preprocessing failed: {}", !vert ? vert.error() : frag.error()));
        return SShaderPair{.vert = std::move(*vert), .frag = std::move(*frag), .builtin = true};
    }();
    return pair;
}

// CShader::createProgram logs compile/link errors and discards the text
// (Shader.cpp logShaderError), so compile and link once ourselves to capture
// the GLSL log, mapped back to file:line. Raw shader/program objects only, no
// cached GL state touched. Returns the error description, or nullopt if both
// stages compile and link.
static std::optional<std::string> glslCheck(const SShaderPair& pair) {
    const auto infoLog = [](GLuint obj, bool program) {
        GLint len = 0;
        program ? glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len) : glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(len, 0), '\0');
        if (len > 0)
            program ? glGetProgramInfoLog(obj, len, &len, log.data()) : glGetShaderInfoLog(obj, len, &len, log.data());
        log.resize(std::max(len, 0));
        while (!log.empty() && (log.back() == '\0' || log.back() == '\n' || log.back() == ' '))
            log.pop_back();
        return log.empty() ? std::string{"(no log from driver)"} : log;
    };

    const auto compile = [&](GLenum type, const hyprtail::shader::SSource& src, std::string& error) -> GLuint {
        const GLuint sh = glCreateShader(type);
        if (!sh) {
            error = "glCreateShader failed";
            return 0;
        }
        const char* p = src.text.c_str();
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = GL_FALSE;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            error = hyprtail::shader::mapLog(infoLog(sh, false), src);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };

    std::string  error;
    const GLuint vs = compile(GL_VERTEX_SHADER, pair.vert, error);
    if (!vs)
        return std::format("vertex shader {} failed to compile:\n{}", pair.vert.sourceNames.front(), error);

    const GLuint fs = compile(GL_FRAGMENT_SHADER, pair.frag, error);
    if (!fs) {
        glDeleteShader(vs);
        return std::format("fragment shader {} failed to compile:\n{}", pair.frag.sourceNames.front(), error);
    }

    std::optional<std::string> result;
    const GLuint               prog = glCreateProgram();
    if (!prog)
        result = "glCreateProgram failed";
    else {
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE)
            result = std::format("shaders {} + {} failed to link (varyings must match):\n{}", pair.vert.sourceNames.front(), pair.frag.sourceNames.front(), infoLog(prog, true));
        glDetachShader(prog, vs);
        glDetachShader(prog, fs);
        glDeleteProgram(prog);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return result;
}

// Compile `pair` and make it the active program. On failure the active
// program is untouched and the error is returned.
static std::optional<std::string> compileAndActivate(STrailInstance& inst, const SShaderPair& pair) {
    if (auto error = glslCheck(pair))
        return error;

    // silent: failures are ours to report; core's error bar would label them
    // "Screen shader parser", which is misleading.
    auto shader = makeShared<CShader>();
    if (!shader->createProgram(pair.vert.text, pair.frag.text, /*dynamic=*/true, /*silent=*/true))
        return std::string{"shader passed a standalone compile/link check but CShader::createProgram failed"};

    auto& s = inst.shader;

    // Make the new program current through Hyprland's cache before deleting
    // the old one, so the cache never holds a deleted program's id.
    g_pHyprOpenGL->useShader(shader);
    if (s.shader)
        s.shader->destroy();

    // Custom uniforms aren't in eShaderUniform, look them up directly. A
    // shader that doesn't declare one gets -1, which glUniform ignores.
    const auto prog     = shader->program();
    s.locNowMs          = glGetUniformLocation(prog, "nowMs");
    s.locFadeMs         = glGetUniformLocation(prog, "fadeMs");
    s.locWidthPx        = glGetUniformLocation(prog, "widthPx");
    s.locMiterLimit     = glGetUniformLocation(prog, "miterLimit");
    s.locSpeedRef       = glGetUniformLocation(prog, "speedRef");
    s.declaredPaddingPx = pair.declaredPaddingPx();
    s.shader            = shader;

    Log::logger->log(Log::INFO, "[hyprtail] {} trail shader active ({} + {}), program id={}", inst.name, pair.vert.sourceNames.front(), pair.frag.sourceNames.front(),
                     prog);
    return std::nullopt;
}

bool trailPrepare(STrailInstance& inst) {
    if (inst.disabled)
        return false;

    const auto key = "shader:" + inst.name;

    if (inst.shader.pending) {
        const SShaderPair pair = std::move(*inst.shader.pending);
        inst.shader.pending.reset();

        if (const auto error = compileAndActivate(inst, pair)) {
            // Report every failed attempt, not just the first of the session.
            hyprtail::diag::resetKey(key);
            const char* keeping = inst.shader.shader ? "keeping the previous shader" : "using the built-in shader";
            hyprtail::diag::report(pair.builtin ? eSeverity::ERR : eSeverity::WARN, key, std::format("{} trail: {}\n{}", inst.name, keeping, *error));
        } else
            hyprtail::diag::resetKey(key);
    }

    if (inst.shader.shader)
        return true;

    // Nothing active (first load, or the first user shader failed): built-in.
    if (const auto error = compileAndActivate(inst, builtinShaderPair())) {
        trailDisable(inst, key, std::format("{} trail disabled: the built-in shader failed: {}", inst.name, *error));
        trailReleaseGpu(inst);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- GPU buffer

static constexpr GLsizei NODE_STRIDE = sizeof(SGpuNode);

bool CTrailGpu::ensure(size_t ringCapacity, std::string& error) {
    if (m_vao && m_vboNodes == ringCapacity + 2)
        return true;

    // Capacity changed (config reload): reallocate.
    if (m_vao)
        destroy();

    // Front and back pad, see header.
    m_vboNodes = ringCapacity + 2;

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    if (!m_vao || !m_vbo) {
        error = std::format("glGenVertexArrays/glGenBuffers returned no name (vao={}, vbo={})", m_vao, m_vbo);
        destroy();
        return false;
    }

    // Drain stale errors so the check after glBufferData is ours. This hides
    // errors core left behind; only core's debug builds look at those.
    while (glGetError() != GL_NO_ERROR) {}

    glBindVertexArray(m_vao);
    // Raw bind is fine at efb5099: there is no array-buffer cache, core binds
    // raw too (OpenGL.cpp:1547, Shader.cpp:236). Later main adds
    // CHyprOpenGLImpl::bindArrayBuffer(); use that if the pin moves past it.
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_vboNodes * NODE_STRIDE, nullptr, GL_DYNAMIC_DRAW);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        error = std::format("glBufferData for {} bytes failed, GL error 0x{:x}", m_vboNodes * NODE_STRIDE, err);
        destroy();
        return false;
    }

    // One per-instance attribute: `components` floats of field at byteOffset
    // within the node, `nodeOffset` nodes into the VBO.
    const auto attrib = [](GLuint loc, GLint components, size_t nodeOffset, size_t fieldOffset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, components, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(nodeOffset * NODE_STRIDE + fieldOffset));
        glVertexAttribDivisor(loc, 1);
    };

    // prev = n(i-1), p0 = n(i), p1 = n(i+1), next = n(i+2); see header.
    attrib(0, 2, 0, offsetof(SGpuNode, posPx));
    attrib(1, 1, 0, offsetof(SGpuNode, flags));
    for (GLuint end = 0; end < 2; ++end) {
        const GLuint loc = 2 + end * 4;
        attrib(loc + 0, 2, 1 + end, offsetof(SGpuNode, posPx));
        attrib(loc + 1, 1, 1 + end, offsetof(SGpuNode, birthMs));
        attrib(loc + 2, 2, 1 + end, offsetof(SGpuNode, velocity));
        attrib(loc + 3, 1, 1 + end, offsetof(SGpuNode, flags));
    }
    attrib(10, 2, 3, offsetof(SGpuNode, posPx));
    attrib(11, 1, 3, offsetof(SGpuNode, flags));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_uploadedGen = UINT64_MAX;
    return true;
}

void CTrailGpu::upload(const CTrailRing& ring) {
    if (!m_vbo || ring.empty() || ring.generation() == m_uploadedGen)
        return;

    // Rebase on the newest node: every uploaded birthMs is <= 0 and small, so
    // float keeps sub-ms precision regardless of how long the plugin has run.
    m_refMs = ring.newest().birthTimeMs;
    ring.orderedCopy(m_ordered, m_refMs);

    // Pads, see header: front = n0 as a segment start, back = newest.
    SGpuNode front = m_ordered.front();
    front.flags    = GPU_FLAG_SEGMENT_START;
    m_ordered.insert(m_ordered.begin(), front);
    m_ordered.push_back(m_ordered.back());

    const size_t n = std::min(m_ordered.size(), m_vboNodes);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * NODE_STRIDE, m_ordered.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_uploadedGen = ring.generation();
}

void CTrailGpu::destroy() {
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_vbo)
        glDeleteBuffers(1, &m_vbo);

    m_vao         = 0;
    m_vbo         = 0;
    m_vboNodes    = 0;
    m_uploadedGen = UINT64_MAX;
    m_refMs       = 0.0;
    m_ordered.clear();
}

GLuint CTrailGpu::vao() const {
    return m_vao;
}

double CTrailGpu::refMs() const {
    return m_refMs;
}

// ---------------------------------------------------------------- instance

CBox trailBoxLocal(const STrailInstance& inst, const STrailBounds& b, const Vector2D& monitorPos) {
    const float r = inst.padPx();

    return CBox{b.x1 - r - monitorPos.x, b.y1 - r - monitorPos.y, (b.x2 - b.x1) + 2.0 * r, (b.y2 - b.y1) + 2.0 * r};
}

void trailDisable(STrailInstance& inst, std::string_view key, std::string_view message) {
    inst.disabled = true;
    hyprtail::diag::report(eSeverity::ERR, key, message);
}

void trailReleaseGpu(STrailInstance& inst) {
    inst.gpu.destroy();
    if (inst.shader.shader) {
        inst.shader.shader->destroy();
        inst.shader.shader.reset();
    }
}

void trailInstanceCleanup(STrailInstance& inst) {
    trailReleaseGpu(inst);
    inst.monState.clear();
}

// ---------------------------------------------------------------- pass element

CTrailPassElement::CTrailPassElement(STrailInstance* inst, const CBox& boxLocal, double nowMs) : m_inst(inst), m_boxLocal(boxLocal), m_nowMs(nowMs) {}

std::optional<CBox> CTrailPassElement::boundingBox() {
    return m_boxLocal;
}

std::vector<UP<IPassElement>> CTrailPassElement::draw() {
    // Called from the pass render inside Hyprland: nothing may escape.
    const bool ok = hyprtail::diag::guard("trail-draw", [this] { drawInternal(); });
    if (!ok && m_inst) {
        m_inst->disabled = true;
        trailReleaseGpu(*m_inst); // GL is current here
    }
    return {};
}

void CTrailPassElement::drawInternal() {
    auto&      rd      = g_pHyprRenderer->m_renderData;
    const auto monitor = rd.pMonitor.lock();
    if (!m_inst || m_inst->disabled || !monitor || m_inst->ring.empty())
        return;

    if (!trailPrepare(*m_inst))
        return;

    if (std::string error; !m_inst->gpu.ensure(m_inst->ring.capacity(), error)) {
        trailDisable(*m_inst, "gl:" + m_inst->name, std::format("{} trail disabled: GL resource creation failed: {}", m_inst->name, error));
        trailReleaseGpu(*m_inst);
        return;
    }

    m_inst->gpu.upload(m_inst->ring);

    // One instance per segment.
    if (m_inst->ring.size() < 2)
        return;
    const auto count = sc<GLsizei>(m_inst->ring.size() - 1);

    // Global layout px -> monitor-local px -> clip. projectBoxToTarget takes a
    // pixel-space box (Renderer.cpp:1842-1846); projectBox maps p to
    // pos + size * p (hyprutils Mat3x3.cpp:68-90), so this box encodes
    // (p - monitorPos) * scale.
    // Transform is passed explicitly as NORMAL: getBoxProjection otherwise
    // defaults to the inverted monitor transform applied *inside* the box
    // (Renderer.cpp:1836-1840), which would rotate this pseudo-box. Monitor
    // rotation stays in targetProjection (Renderer.cpp:1828). Rotated outputs
    // are untested.
    const double s    = monitor->m_scale;
    const auto   proj = g_pHyprRenderer->projectBoxToTarget(CBox{-monitor->m_position.x * s, -monitor->m_position.y * s, s, s}, HYPRUTILS_TRANSFORM_NORMAL);

    // Through Hyprland's program cache (OpenGL.cpp:2561-2569), not raw glUseProgram.
    auto shader = g_pHyprOpenGL->useShader(m_inst->shader.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, proj.getMatrix());
    glUniform1f(m_inst->shader.locNowMs, sc<float>(m_nowMs - m_inst->gpu.refMs()));
    glUniform1f(m_inst->shader.locFadeMs, sc<float>(m_inst->fadeMs));
    glUniform1f(m_inst->shader.locWidthPx, m_inst->widthPx);
    glUniform1f(m_inst->shader.locMiterLimit, m_inst->miterLimit);
    glUniform1f(m_inst->shader.locSpeedRef, m_inst->speedRefPxPerMs);

    // Premultiplied blending through Hyprland's cap-status cache (OpenGL.cpp:981-989).
    // Blend state is whatever the previous element left, so set it explicitly.
    g_pHyprOpenGL->blend(true);

    glBindVertexArray(m_inst->gpu.vao());

    // Clip to this element's damage, same pattern as core (OpenGL.cpp:1117-1124).
    // Scissor through Hyprland's cached state, not raw glEnable/glScissor.
    rd.damage.forEachRect([&rd, count](const auto& RECT) {
        g_pHyprOpenGL->scissor(&RECT, rd.transformDamage);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
    });

    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
}
