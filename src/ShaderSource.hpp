#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Params.hpp"

// Loader-side GLSL preprocessing (SPEC §5, contract 2 in §13). GLSL ES has no
// #include, so the plugin resolves it before handing source to the driver:
//
//   #pragma hyprtail contract 2  required, right after #version, before any
//                                other hyprtail pragma or #include; replaced
//                                by the prelude (shaders/prelude/)
//   #pragma hyprtail topology <path|quad>
//                                geometry (vertex) shaders only, main file,
//                                exactly once
//   #pragma hyprtail param <type> <name> <default> [<min> <max>]
//                                replaced by "uniform <type> <name>;"
//   #pragma hyprtail padding <expr>
//                                this program's reach past the node
//                                positions (quad: past the anchor)
//   #include "hyprtail/<name>"   built-in prefab library (embedded)
//   #include "<path>"            relative to the including file; absolute and
//                                ~/ paths also work
//
// Each file is included at most once; cycles are errors; depth is limited.
// Included files must not contain #version, contract or topology. Every
// inclusion (and the prelude) is wrapped in "#line <n> <source-id>" so driver
// errors map back to file:line (mapLog).
namespace hyprtail::shader {
    enum class eStage : uint8_t {
        VERTEX,
        FRAGMENT,
    };

    enum class eTopology : uint8_t {
        PATH,
        QUAD,
    };

    constexpr int CONTRACT_VERSION = 2;

    const char* topologyName(eTopology t);

    struct SPadding {
        params::CExpr expr;
        std::string   where; // file:line, for messages
    };

    struct SParamWhere {
        params::SDecl decl;
        std::string   where;
    };

    struct SSource {
        std::string                        text;
        std::vector<std::string>           sourceNames; // index = GLSL source-string number
        std::vector<std::filesystem::path> files;       // real files read, for watching

        std::optional<eTopology>           topology; // vertex stage only
        std::vector<SParamWhere>           params;
        std::vector<SPadding>              padding;
    };

    // Built-in shaders (the classic preset), embedded at build time, by name,
    // e.g. "classic/ribbon.vert". Empty view if unknown.
    std::string_view builtin(std::string_view name);

    // Preprocess a main shader. `name` is for messages. `path` empty means a
    // built-in shader: it may only include hyprtail/ prefabs.
    std::expected<SSource, std::string> preprocess(std::string_view mainText, const std::string& name, const std::filesystem::path& path, eStage stage);

    // Read and preprocess a shader file.
    std::expected<SSource, std::string> load(const std::filesystem::path& path, eStage stage);

    // Rewrite "<source-id>:<line>" references in a driver info log to
    // "<file>:<line>", best effort (drivers format logs differently).
    std::string mapLog(const std::string& log, const SSource& src);

    // What the prelude provides, for the program contract check.
    const std::vector<std::string>& preludeUniforms();
    const std::vector<int>&         preludeAttribLocations(eTopology t);

    // Reserved parameter names: lifecycle values every layer has (the float
    // ones are also prelude uniforms). Shaders can't declare these.
    struct SReserved {
        params::SDecl decl;
        bool          uniform; // declared by the prelude
    };
    const std::vector<SReserved>& reservedParams();
}
