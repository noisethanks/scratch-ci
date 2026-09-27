#include "ShaderSlot.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <regex>
#include <sstream>
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
        struct SContract {
            std::vector<std::string> uniforms;
            std::vector<int>         attribLocations;
        };

        std::optional<std::string> contractCheck(GLuint prog, const SContract& contract) {
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
                    problems.push_back(std::format("uniform `{}` is not provided by the plugin (it would stay 0); declare it with #pragma hyprtail param", n));
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

            std::string out = "shader uses inputs the plugin doesn't provide:";
            for (const auto& p : problems)
                out += "\n  " + p;
            return out;
        }

        // Text-only scan for the pre-link varying check (SPEC §13.6): which
        // fragment `in` declarations aren't matched by a vertex `out` of the
        // same name and type. Regex-based, not real GLSL parsing (misses a
        // multi-name declaration or a layout() qualifier) — deliberately
        // consulted only after a real link failure (see glslCheck below), so
        // a false negative here just falls through to the raw driver log,
        // and there's no false-positive risk of it ever blocking or skipping
        // an actual compile/link attempt.
        std::optional<std::string> varyingCheck(const SShaderPair& pair) {
            static const std::regex RE_OUT{R"re(^\s*(?:flat\s+)?out\s+(\w+)\s+(\w+)\s*;\s*$)re"};
            static const std::regex RE_IN{R"re(^\s*(?:flat\s+)?in\s+(\w+)\s+(\w+)\s*;\s*$)re"};

            const auto scan = [](const std::string& text, const std::regex& re) {
                std::map<std::string, std::string> out; // name -> type
                std::istringstream                 in{text};
                std::string                         line;
                std::smatch                          m;
                while (std::getline(in, line))
                    if (std::regex_match(line, m, re))
                        out.emplace(m[2].str(), m[1].str());
                return out;
            };

            const auto vertOuts = scan(pair.vert.text, RE_OUT);
            const auto fragIns  = scan(pair.frag.text, RE_IN);

            static const std::string STANDARD = "ht_vLocal, ht_vAge, ht_vLife, ht_vSpeed, ht_vDist, ht_vSeed";
            std::vector<std::string> problems;
            for (const auto& [name, fragType] : fragIns) {
                const auto it = vertOuts.find(name);
                if (it == vertOuts.end())
                    problems.push_back(std::format("fragment shader {} reads `{}`, which geometry shader {} doesn't write (standard varyings: {})",
                                                   pair.frag.sourceNames.front(), name, pair.vert.sourceNames.front(), STANDARD));
                else if (it->second != fragType)
                    problems.push_back(std::format("fragment shader {} declares `{}` as {}; geometry shader {} declares it as {}", pair.frag.sourceNames.front(), name,
                                                   fragType, pair.vert.sourceNames.front(), it->second));
            }
            if (problems.empty())
                return std::nullopt;

            std::string out = "varying mismatch:";
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
        std::optional<std::string> glslCheck(const SShaderPair& pair, const SContract& contract) {
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
                if (ok != GL_TRUE) {
                    // The real link attempt always runs first and unconditionally;
                    // varyingCheck only replaces the message on an actual failure,
                    // never skips or blocks the attempt itself.
                    if (auto mismatch = varyingCheck(pair))
                        result = std::format("shaders {} + {} failed to link: {}", pair.vert.sourceNames.front(), pair.frag.sourceNames.front(), *mismatch);
                    else
                        result = std::format("shaders {} + {} failed to link (varyings must match):\n{}", pair.vert.sourceNames.front(), pair.frag.sourceNames.front(),
                                             infoLog(prog, true));
                } else if (auto bad = contractCheck(prog, contract))
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

    namespace {
        // The declarations of both stages have to fit together: one
        // topology (vertex), parameters declared identically wherever they
        // appear, padding expressions over known scalar parameters.
        std::expected<SProgramInfo, std::string> programInfo(const SShaderPair& pair) {
            SProgramInfo info;
            info.topology = pair.vert.topology.value_or(shader::eTopology::PATH);

            // #pragma hyprtail expects (SPEC §13.3): the fragment shader's
            // declared topology compatibility against what the vertex
            // shader actually provides.
            if (!pair.frag.expects.empty() && std::ranges::find(pair.frag.expects, info.topology) == pair.frag.expects.end()) {
                std::string kinds;
                for (const auto& k : pair.frag.expects)
                    kinds += (kinds.empty() ? "" : ",") + std::string{shader::topologyName(k)};
                return std::unexpected(std::format("{} expects topology {}; {} declares {}", pair.frag.sourceNames.front(), kinds, pair.vert.sourceNames.front(),
                                                   shader::topologyName(info.topology)));
            }

            for (const auto* src : {&pair.vert, &pair.frag}) {
                for (const auto& p : src->params) {
                    const auto it = std::ranges::find_if(info.params, [&](const auto& d) { return d.name == p.decl.name; });
                    if (it == info.params.end()) {
                        info.params.push_back(p.decl);
                        continue;
                    }
                    const bool same = it->type == p.decl.type && params::format(it->def) == params::format(p.decl.def) && it->min == p.decl.min && it->max == p.decl.max;
                    if (!same)
                        return std::unexpected(std::format("param {} is declared differently in the two stages ({}); declare it identically", p.decl.name, p.where));
                }
            }

            for (const auto* src : {&pair.vert, &pair.frag}) {
                for (const auto& pad : src->padding) {
                    for (const auto& n : pad.expr.names()) {
                        const bool param = std::ranges::any_of(info.params, [&](const auto& d) { return d.name == n && d.type != params::eType::VEC2 && d.type != params::eType::COLOR; });
                        const bool reserved = std::ranges::any_of(shader::reservedParams(), [&](const auto& r) { return r.decl.name == n; });
                        if (!param && !reserved)
                            return std::unexpected(std::format("{}: padding uses \"{}\", which isn't a float, int or bool param of this program", pad.where, n));
                    }
                    info.padding.push_back(pad);
                }
            }
            return info;
        }
    }

    CShaderSlot::CShaderSlot(std::string name, std::string vertBuiltin, std::string fragBuiltin) :
        m_name(std::move(name)), m_vertBuiltin(std::move(vertBuiltin)), m_fragBuiltin(std::move(fragBuiltin)) {}

    const SShaderPair& CShaderSlot::builtin() {
        if (!m_builtin) {
            auto vert = shader::preprocess(shader::builtin(m_vertBuiltin), m_vertBuiltin, {}, shader::eStage::VERTEX);
            auto frag = shader::preprocess(shader::builtin(m_fragBuiltin), m_fragBuiltin, {}, shader::eStage::FRAGMENT);
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

        const auto  loadStage = [&](const std::string& configured, const char* stage, shader::eStage st, shader::SSource& out, std::string& origin) {
            const auto path = cfg::resolveShaderPath(configured);
            if (path.empty())
                return; // built-in

            origin = path.string();
            watch.push_back(path); // even if missing, so creating it reloads
            auto src = shader::load(path, st);
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

        loadStage(vertConfigured, "vertex", shader::eStage::VERTEX, pair.vert, pair.vertOrigin);
        loadStage(fragConfigured, "fragment", shader::eStage::FRAGMENT, pair.frag, pair.fragOrigin);

        if (!failed)
            m_pending = std::move(pair);
        else
            m_lastResult = "file error, kept the current shader";
    }

    std::optional<std::string> CShaderSlot::compileAndActivate(const SShaderPair& pair) {
        auto info = programInfo(pair);
        if (!info)
            return std::format("shaders {} + {}: {}", pair.vert.sourceNames.front(), pair.frag.sourceNames.front(), info.error());

        SContract contract{.uniforms = shader::preludeUniforms(), .attribLocations = shader::preludeAttribLocations(info->topology)};
        for (const auto& p : info->params)
            contract.uniforms.push_back(p.name);

        if (auto error = glslCheck(pair, contract))
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
        m_info              = std::move(*info);
        ++m_generation;
        m_activeOrigin      = pair.vertOrigin + '\n' + pair.fragOrigin;
        m_activeVertOrigin  = pair.vertOrigin;
        m_activeFragOrigin  = pair.fragOrigin;
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
                m_lastResult = sameFiles ? "failed, kept the last working version" : "failed, using the built-in shader";
            } else {
                diag::resetKey(key);
                m_lastResult = "ok";
            }
        }

        if (m_shader)
            return std::nullopt;

        // Nothing active (first load, or the first user shader failed).
        if (const auto error = compileAndActivate(builtin())) {
            m_lastResult = "built-in shader failed";
            return std::format("the built-in shader failed: {}", *error);
        }
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

    const SProgramInfo& CShaderSlot::info() const {
        return m_info;
    }

    uint64_t CShaderSlot::generation() const {
        return m_generation;
    }

    GLint CShaderSlot::loc(const std::string& uniform) {
        if (!m_shader)
            return -1;
        const auto it = m_locs.find(uniform);
        if (it != m_locs.end())
            return it->second;
        // Custom uniforms aren't in eShaderUniform, look them up directly.
        const GLint l = glGetUniformLocation(m_shader->program(), uniform.c_str());
        m_locs.emplace(uniform, l);
        return l;
    }

    const std::string& CShaderSlot::name() const {
        return m_name;
    }

    bool CShaderSlot::hasPending() const {
        return m_pending.has_value();
    }

    SSlotStatus CShaderSlot::status() const {
        return {
            .active     = m_shader != nullptr,
            .pending    = m_pending.has_value(),
            .topology   = m_shader ? shader::topologyName(m_info.topology) : "",
            .vertOrigin = m_activeVertOrigin,
            .fragOrigin = m_activeFragOrigin,
            .lastResult = m_lastResult,
        };
    }
}
