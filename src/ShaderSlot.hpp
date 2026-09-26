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

    // Which files the pair came from ("" = built-in stage), resolved. Used to
    // tell an edit of the same files (keep the previous program on failure)
    // from a config change to other files (fall back to built-in).
    std::string               vertOrigin, fragOrigin;

    float                     declaredPaddingPx() const {
        return std::max(vert.declaredPaddingPx, frag.declaredPaddingPx);
    }
};

namespace hyprtail {
    // What the plugin provides to a slot's shaders. A shader that uses
    // anything else would silently read zeros (e.g. a shader written for a
    // newer plugin version), so it's rejected at compile time.
    struct SShaderContract {
        std::vector<std::string> uniforms;         // names the plugin sets
        std::vector<GLint>       attribLocations; // locations the plugin feeds
    };

    // What `hyprctl hyprtail` shows for a slot.
    struct SSlotStatus {
        bool        active  = false; // a program is in use
        bool        pending = false; // a reloaded pair waits for the next render
        std::string vertOrigin, fragOrigin; // active program's files, "" = built-in
        std::string lastResult; // outcome of the last compile attempt
    };

    // One user-replaceable shader program (SPEC section 5): built-in stages,
    // optional user files per stage, include preprocessing, deferred compile
    // on the next render, and "keep the previous working program" on failure.
    // Shared by the trail and the idle slot. Reports under shader:<name>.
    class CShaderSlot {
      public:
        CShaderSlot(std::string name, std::string vertName, std::string_view vertBuiltin, std::string fragName, std::string_view fragBuiltin, SShaderContract contract);

        // Main thread (config reload, file change). Reads each configured
        // stage (empty = built-in), resolves includes, and queues the pair for
        // the next prepare(). A file or preprocessing error is reported and
        // keeps the active program. Appends every file to watch (including
        // missing ones, so creating them reloads).
        void reload(const std::string& vertConfigured, const std::string& fragConfigured, std::vector<std::filesystem::path>& watch);

        // Inside a render (GL current): compile a pending pair. On failure
        // (compile/link error, or it uses uniforms/attributes the contract
        // doesn't provide) it's reported and:
        //   - same files as the active program (an edit): keep the active one;
        //   - other files (a config change) or nothing active: built-in.
        // Returns an error only if no program is usable at all (the built-in
        // one failed).
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

        bool               hasPending() const;
        SSlotStatus        status() const;

      private:
        const SShaderPair&         builtin();
        std::optional<std::string> compileAndActivate(const SShaderPair& pair);
        std::optional<std::string> checkContract(GLuint program) const;

        std::string                            m_name, m_vertName, m_fragName;
        std::string_view                       m_vertBuiltin, m_fragBuiltin;
        SShaderContract                        m_contract;
        std::string                            m_activeOrigin; // vertOrigin + '\n' + fragOrigin of the active program
        std::string                            m_activeVertOrigin, m_activeFragOrigin;
        std::string                            m_lastResult = "none yet";
        std::optional<SShaderPair>             m_builtin;
        std::optional<SShaderPair>             m_pending;
        SP<CShader>                            m_shader;
        float                                  m_declaredPaddingPx = 0.F;
        std::unordered_map<std::string, GLint> m_locs;
    };
}
