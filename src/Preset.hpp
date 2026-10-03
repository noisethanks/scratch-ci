#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Layer.hpp"
#include "Source.hpp"

// Preset manifests (SPEC §13.7): the preset file format, plain "key = value"
// lines, '#' to end-of-line comments, blank lines ignored. Layer keys are prefixed
// "<layer>:", as in "plugin:hyprtail:...".
//
//   contract    = 2                          required, exactly once
//   description = ...                        optional, at most once
//   layers      = <name>[, <name>...]         required, 1-4, no duplicates
//   source      = pointer | spring            optional, at most once, default pointer
//
//   source:<name>    = <value>                a setting of the source (Source.hpp)
//   <layer>:vertex   = prefab:<built-in name> or a path
//   <layer>:fragment = prefab:<built-in name> or a path
//   <layer>:<name>   = <value>                a parameter default
//
// Two namespaces, no shadowing: "prefab:<name>" is an embedded built-in
// preset; a bare "<name>" is <hyprtail root>/presets/<name>.conf
// (cfg::hyprtailRoot(): $XDG_CONFIG_HOME/hypr/hyprtail, fallback
// ~/.config/hypr/hyprtail). Shader stages inside a manifest use the same
// split: "prefab:<name>" embedded, anything else a path (user presets only),
// relative ones against the hyprtail root.
//
// The source is the one thing every layer of a preset draws from, so it is
// declared once, by the preset, and its settings live under the reserved
// layer name "source" (a layer can't be called that).
namespace hyprtail::preset {
    // Manifest grammar version, unrelated to shader::CONTRACT_VERSION (§5):
    // this one just lets a future breaking change to the preset file format's own
    // syntax name itself, the same way shader contracts do.
    constexpr int CONTRACT_VERSION = 2;

    // Parses the plain-text grammar only: purely structural checks (unknown
    // top-level key, a "<layer>:" key for a layer not in `layers`, a bad or
    // missing contract, `layers` empty/duplicated/over 4). What "vertex" and
    // "fragment" resolve to, and whether a parameter name the manifest sets
    // is one the layer's shader actually declares, need the shader itself
    // (a built-in name lookup, or a compiled program's declared params) and
    // are checked one level up, in load() and CLayer::resolve() respectively.
    struct SManifest {
        std::string                                               description;
        std::vector<std::string>                                  layers;    // draw order, first = bottom
        std::map<std::string, std::map<std::string, std::string>> layerKeys; // layer name -> (key -> raw value text, incl. "vertex"/"fragment")
        std::string                                               sourceKind{source::DEFAULT_KIND};
        std::map<std::string, std::string>                        sourceKeys; // "source:<name>" -> raw value text

        bool                                                      operator==(const SManifest&) const = default;
    };

    std::expected<SManifest, std::string> parse(std::string_view text);

    // A resolved preset, ready for CLayer construction: name, description,
    // and up to 4 SLayerSpecs with vertex/fragment already resolved to a
    // built-in shader::builtin() name or an absolute path.
    struct SResolved {
        std::string                        name, description;
        std::vector<SLayerSpec>            layers;
        std::string                        sourceKind{source::DEFAULT_KIND};
        std::map<std::string, std::string> sourceDefaults; // source settings from the manifest

        bool                               operator==(const SResolved&) const = default;
    };

    // Resolves `name` ("prefab:<name>" or a bare "<name>", see above). A
    // bare name that has no file is an error, never a built-in. Any failure
    // (not found, parse error, an unrecognized prefab shader, a path in a
    // prefab preset, an unresolvable path in a user preset) is reported
    // (diag, "preset:<name>") and falls back to the embedded
    // "prefab:subtle" manifest, which is guaranteed to parse -- it ships
    // with the plugin.
    SResolved load(const std::string& name);
}
