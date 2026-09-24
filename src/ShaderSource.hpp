#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Loader-side GLSL preprocessing (SPEC section 5). GLSL ES has no #include,
// so the plugin resolves it before handing source to the driver:
//
//   #include "hyprtail/<name>"   built-in prefab library (embedded)
//   #include "<path>"            relative to the including file; absolute and
//                                ~/ paths also work
//   #pragma hyprtail padding <px>
//                                extra damage padding this shader needs
//                                beyond the stock ribbon extent; the largest
//                                one across a program's files counts
//
// Each file is included at most once; cycles are errors; depth is limited.
// Included files must not contain #version. Every inclusion is wrapped in
// "#line <n> <source-id>" so driver errors can be mapped back to file:line
// (mapLog).
namespace hyprtail::shader {
    struct SSource {
        std::string                        text;
        float                              declaredPaddingPx = 0.F;
        std::vector<std::string>           sourceNames; // index = GLSL source-string number
        std::vector<std::filesystem::path> files;       // real files read, for watching
    };

    // Built-in stock shaders, embedded at build time.
    std::string_view builtinVertex();       // trail
    std::string_view builtinFragment();     // trail
    std::string_view builtinIdleVertex();   // idle/presence slot
    std::string_view builtinIdleFragment(); // idle/presence slot

    // Preprocess a main shader. `name` is for messages. `path` empty means a
    // built-in shader: it may only include hyprtail/ prefabs.
    std::expected<SSource, std::string> preprocess(std::string_view mainText, const std::string& name, const std::filesystem::path& path);

    // Read and preprocess a shader file.
    std::expected<SSource, std::string> load(const std::filesystem::path& path);

    // Rewrite "<source-id>:<line>" references in a driver info log to
    // "<file>:<line>", best effort (drivers format logs differently).
    std::string mapLog(const std::string& log, const SSource& src);
}
