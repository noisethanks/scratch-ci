#include "ShaderSlot.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>

#include <render/OpenGL.hpp>
#include <debug/log/Logger.hpp>

#include "Config.hpp"
#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;
using namespace Render::GL;

namespace hyprtail {
    namespace {
        // Active uniforms and attributes the program uses that the plugin
        // doesn't provide. Such a shader compiles and links but reads zeros
        // there (e.g. a shader written for a newer plugin version reading
        // palette uniforms an older plugin never sets draws fully
        // transparent), so it's treated as a failure.
        std::optional<std::string> contractCheck(GLuint prog, const SShaderContract& contract) {
            std::vector<std::string> problems;
            std::array<char, 256>    name{};

            GLint                    count = 0;
            glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
            for (GLint i = 0; i < count; ++i) {
                GLsizei len  = 0;
                GLint   size = 0;
                GLenum  type = 0;
                glGetActiveUniform(prog, sc<GLuint>(i), name.size(), &len, &size, &type, name.data());
                std::string n{name.data(), sc<size_t>(std::max(len, 0))};
                if (n.starts_with("gl_"))
                    continue;
                if (std::ranges::find(contract.uniforms, n) == contract.uniforms.end())
                    problems.push_back(std::format("uniform `{}` is not provided by this hyprtail version (it would stay 0)", n));
            }

            count = 0;
            glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &count);
            for (GLint i = 0; i < count; ++i) {
                GLsizei len  = 0;
                GLint   size = 0;
                GLenum  type = 0;
                glGetActiveAttrib(prog, sc<GLuint>(i), name.size(), &len, &size, &type, name.data());
                std::string n{name.data(), sc<size_t>(std::max(len, 0))};
                if (n.starts_with("gl_"))
                    continue;
                const GLint loc = glGetAttribLocation(prog, n.c_str());
                if (std::ranges::find(contract.attribLocations, loc) == contract.attribLocations.end())
                    problems.push_back(std::format("attribute `{}` at location {} is not fed by this hyprtail version", n, loc));
            }

            if (problems.empty())
                return std::nullopt;

            std::string out = "shader uses inputs this hyprtail version doesn't provide (written for another version? see the contract in the stock shaders):";
            for (const auto& p : problems)
                out += "\n  " + p;
            return out;
        }

        // CShader::createProgram logs compile/link errors and discards the
        // text (Shader.cpp logShaderError), so compile and link once ourselves
        // to capture the GLSL log, mapped back to file:line, and to check the
        // program against the slot's contract. Raw shader/program objects
        // only, no cached GL state touched. Returns the error, or nullopt if
        // both stages compile, link and fit the contract.
        std::optional<std::string> glslCheck(const SShaderPair& pair, const SShaderContract& contract) {
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

            const auto compile = [&](GLenum type, const shader::SSource& src, std::string& error) -> GLuint {
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
                    error = shader::mapLog(infoLog(sh, false), src);
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
                    result = std::format("shaders {} + {} failed to link (varyings must match):\n{}", pair.vert.sourceNames.front(), pair.frag.sourceNames.front(),
                                         infoLog(prog, true));
                else if (auto bad = contractCheck(prog, contract))
                    result = std::format("shaders {} + {}: {}", pair.vert.sourceNames.front(), pair.frag.sourceNames.front(), *bad);
                glDetachShader(prog, vs);
                glDetachShader(prog, fs);
                glDeleteProgram(prog);
            }

            glDeleteShader(vs);
            glDeleteShader(fs);
            return result;
        }
    }

    CShaderSlot::CShaderSlot(std::string name, std::string vertName, std::string_view vertBuiltin, std::string fragName, std::string_view fragBuiltin,
                             SShaderContract contract) :
        m_name(std::move(name)), m_vertName(std::move(vertName)), m_fragName(std::move(fragName)), m_vertBuiltin(vertBuiltin), m_fragBuiltin(fragBuiltin),
        m_contract(std::move(contract)) {}

    const SShaderPair& CShaderSlot::builtin() {
        if (!m_builtin) {
            auto vert = shader::preprocess(m_vertBuiltin, m_vertName, {});
            auto frag = shader::preprocess(m_fragBuiltin, m_fragName, {});
            // Built-ins are fixed at build time; a failure here is a plugin bug.
            if (!vert || !frag)
                throw std::runtime_error(std::format("{}: built-in shader preprocessing failed: {}", m_name, !vert ? vert.error() : frag.error()));
            m_builtin = SShaderPair{.vert = std::move(*vert), .frag = std::move(*frag), .builtin = true};
        }
        return *m_builtin;
    }

    void CShaderSlot::reload(const std::string& vertConfigured, const std::string& fragConfigured, std::vector<std::filesystem::path>& watch) {
        const auto  key = "shader:" + m_name;

        SShaderPair pair   = builtin();
        bool        failed = false;

        const auto  loadStage = [&](const std::string& configured, const char* stage, shader::SSource& out, std::string& origin) {
            const auto path = cfg::resolveShaderPath(configured);
            if (path.empty())
                return; // built-in

            origin = path.string();
            watch.push_back(path); // even if missing, so creating it reloads
            auto src = shader::load(path);
            if (!src) {
                diag::resetKey(key);
                diag::report(eSeverity::WARN, key, std::format("{}: {} shader: {}\nKeeping the current shader.", m_name, stage, src.error()));
                failed = true;
                return;
            }
            watch.insert(watch.end(), src->files.begin(), src->files.end());
            out          = std::move(*src);
            pair.builtin = false;
        };

        loadStage(vertConfigured, "vertex", pair.vert, pair.vertOrigin);
        loadStage(fragConfigured, "fragment", pair.frag, pair.fragOrigin);

        if (!failed)
            m_pending = std::move(pair);
    }

    std::optional<std::string> CShaderSlot::compileAndActivate(const SShaderPair& pair) {
        if (auto error = glslCheck(pair, m_contract))
            return error;

        // silent: failures are ours to report; core's error bar would label
        // them "Screen shader parser", which is misleading.
        auto shader = makeShared<CShader>();
        if (!shader->createProgram(pair.vert.text, pair.frag.text, /*dynamic=*/true, /*silent=*/true))
            return std::string{"shader passed a standalone compile/link check but CShader::createProgram failed"};

        // Make the new program current through Hyprland's cache before
        // deleting the old one, so the cache never holds a deleted program id.
        g_pHyprOpenGL->useShader(shader);
        if (m_shader)
            m_shader->destroy();

        m_shader            = shader;
        m_declaredPaddingPx = pair.declaredPaddingPx();
        m_activeOrigin      = pair.vertOrigin + '\n' + pair.fragOrigin;
        m_locs.clear();

        Log::logger->log(Log::INFO, "[hyprtail] {} shader active ({} + {}), program id={}", m_name, pair.vert.sourceNames.front(), pair.frag.sourceNames.front(),
                         shader->program());
        return std::nullopt;
    }

    std::optional<std::string> CShaderSlot::prepare() {
        const auto key = "shader:" + m_name;

        if (m_pending) {
            const SShaderPair pair = std::move(*m_pending);
            m_pending.reset();

            if (const auto error = compileAndActivate(pair)) {
                // Keep the active program only if it came from the same files
                // (an edit with a mistake in it: the last working version of
                // those files stays). After a config change to other files,
                // the active program no longer reflects the config and may
                // not even draw, so fall back to the built-in one.
                const bool sameFiles = m_shader && m_activeOrigin == pair.vertOrigin + '\n' + pair.fragOrigin;
                if (m_shader && !sameFiles) {
                    m_shader->destroy();
                    m_shader.reset();
                    m_locs.clear();
                }

                // Report every failed attempt, not just the first of the session.
                diag::resetKey(key);
                const char* fallback = sameFiles ? "keeping the last working version of these files" : "using the built-in shader";
                diag::report(pair.builtin ? eSeverity::ERR : eSeverity::WARN, key, std::format("{}: {}\n{}", m_name, fallback, *error));
            } else
                diag::resetKey(key);
        }

        if (m_shader)
            return std::nullopt;

        // Nothing active (first load, or the first user shader failed).
        if (const auto error = compileAndActivate(builtin()))
            return std::format("the built-in shader failed: {}", *error);
        return std::nullopt;
    }

    void CShaderSlot::release() {
        if (m_shader) {
            m_shader->destroy();
            m_shader.reset();
        }
        m_locs.clear();
    }

    const SP<CShader>& CShaderSlot::shader() const {
        return m_shader;
    }

    float CShaderSlot::declaredPaddingPx() const {
        return m_declaredPaddingPx;
    }

    GLint CShaderSlot::loc(const char* uniform) {
        if (!m_shader)
            return -1;
        const auto it = m_locs.find(uniform);
        if (it != m_locs.end())
            return it->second;
        // Custom uniforms aren't in eShaderUniform, look them up directly.
        const GLint l = glGetUniformLocation(m_shader->program(), uniform);
        m_locs.emplace(uniform, l);
        return l;
    }

    const std::string& CShaderSlot::name() const {
        return m_name;
    }
}
