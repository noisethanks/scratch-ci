#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <helpers/memory/Memory.hpp>
#include <render/Shader.hpp>

#include "ShaderSource.hpp"

// Preprocessed vertex + fragment source for one program.
struct SShaderPair {
    hyprtail::shader::SSource vert, frag;
    bool                      builtin = true; // both stages built-in

    float                     declaredPaddingPx() const {
        return std::max(vert.declaredPaddingPx, frag.declaredPaddingPx);
    }
};

namespace hyprtail {
    // One user-replaceable shader program (SPEC section 5): built-in stages,
    // optional user files per stage, include preprocessing, deferred compile
    // on the next render, and "keep the previous working program" on failure.
    // Shared by the trail and the idle slot. Reports under shader:<name>.
    class CShaderSlot {
      public:
        CShaderSlot(std::string name, std::string vertName, std::string_view vertBuiltin, std::string fragName, std::string_view fragBuiltin);

        // Main thread (config reload, file change). Reads each configured
        // stage (empty = built-in), resolves includes, and queues the pair for
        // the next prepare(). A file or preprocessing error is reported and
        // keeps the active program. Appends every file to watch (including
        // missing ones, so creating them reloads).
        void reload(const std::string& vertConfigured, const std::string& fragConfigured, std::vector<std::filesystem::path>& watch);

        // Inside a render (GL current): compile a pending pair (on failure:
        // report, keep the active program), else fall back to the built-in
        // pair if nothing is active. Returns an error only if no program is
        // usable at all (the built-in one failed).
        std::optional<std::string> prepare();

        // GL context must be current.
        void               release();

        const SP<CShader>& shader() const;
        float              declaredPaddingPx() const;

        // Uniform location in the active program, cached until the next
        // program swap. -1 if the shader doesn't declare it (glUniform ignores
        // -1, so undeclared uniforms are simply not set).
        GLint              loc(const char* uniform);

        const std::string& name() const;

      private:
        const SShaderPair&         builtin();
        std::optional<std::string> compileAndActivate(const SShaderPair& pair);

        std::string                            m_name, m_vertName, m_fragName;
        std::string_view                       m_vertBuiltin, m_fragBuiltin;
        std::optional<SShaderPair>             m_builtin;
        std::optional<SShaderPair>             m_pending;
        SP<CShader>                            m_shader;
        float                                  m_declaredPaddingPx = 0.F;
        std::unordered_map<std::string, GLint> m_locs;
    };
}
