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
//                                by the prelude (hyprtail/shaders/prelude/)
//   #pragma hyprtail topology <path|quad|instanced <K>>
//                                geometry (vertex) shaders only, main file,
//                                exactly once. K (instanced only) is an
//                                integer literal 1..64 or the name of an int
//                                param declared with a range inside 1..64;
//                                the layer draws K quads per visible node.
//   #pragma hyprtail expects <kind>[,<kind>...]
//                                shading (fragment) shaders only, main file,
//                                at most once; no spaces around the commas.
//                                Kinds: path, quad, instanced (no K).
//                                Refused if the paired vertex shader's
//                                topology isn't one of the listed kinds.
//   #pragma hyprtail param <type> <name> <default> [<min> <max>]
//                                replaced by "uniform <type> <name>;"
//   #pragma hyprtail padding <expr>
//                                this program's reach past the node
//                                positions (quad: past the anchor)
//   #include "helpers/<name>"    built-in helper library (embedded, immutable)
//   #include "<path>"            relative to the including file; absolute and
//                                ~/ paths also work. Your own copy of a
//                                helper is "./helpers/<name>": the bare
//                                "helpers/" prefix is always the built-in.
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
        INSTANCED,
    };

    constexpr int CONTRACT_VERSION = 2;
    constexpr int MAX_INSTANCES    = 64; // K of an instanced topology

    // K of "topology instanced <K>": a literal (param empty), or the name of
    // an int param whose value is read at draw time (literal 0).
    struct SInstanceCount {
        int         literal = 0;
        std::string param;
    };

    const char* topologyName(eTopology t);

    // For messages and status: "path", "quad", "instanced 8" or "instanced
    // <param>".
    std::string topologyText(eTopology t, const SInstanceCount& k);

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

        std::optional<eTopology>           topology;     // vertex stage only
        SInstanceCount                     instances;    // vertex stage, instanced topology only
        std::vector<eTopology>             expects;      // fragment stage only; empty = accepts any
        std::string                        expectsWhere; // file:line, for messages
        std::vector<SParamWhere>           params;
        std::vector<SPadding>              padding;
    };

    // Built-in shaders (the prefab presets'), embedded at build time, keyed
    // by their path relative to the hyprtail root, e.g. "shaders/taper.vert"
    // -- the same string a preset's `<layer>:vertex` holds, so a prefab
    // preset's shader reference is looked up here as written. A user preset
    // may also say "prefab:taper.vert", which is "shaders/taper.vert" here.
    // Empty view if unknown.
    std::string_view builtin(std::string_view name);

    // Preprocess a main shader. `name` is for messages. `path` empty means a
    // built-in shader: it may only include helpers/ built-ins.
    std::expected<SSource, std::string> preprocess(std::string_view mainText, const std::string& name, const std::filesystem::path& path, eStage stage);

    // Read and preprocess a shader file.
    std::expected<SSource, std::string> load(const std::filesystem::path& path, eStage stage);

    // Rewrite "<source-id>:<line>" references in a driver info log to
    // "<file>:<line>", best effort (drivers format logs differently).
    std::string mapLog(const std::string& log, const SSource& src);

    // Pair checks, run when a program's two stages are put together (both
    // nullopt on success, else a plain message):
    //
    // The fragment shader's `expects` against the vertex shader's topology.
    std::optional<std::string> expectsMismatch(const SSource& vert, const SSource& frag);

    // An instanced topology's K parameter against the program's merged
    // parameters: declared, int, with a min and max inside 1..MAX_INSTANCES.
    // Nothing to check for a literal K or another topology.
    std::optional<std::string> instanceCountProblem(const SSource& vert, const std::vector<params::SDecl>& programParams);

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
