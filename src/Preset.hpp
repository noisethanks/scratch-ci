#pragma once

#include <expected>
#include <map>
#include <optional>
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
//   <layer>:vertex   = a shader path, e.g. shaders/ribbon.vert (or prefab:ribbon.vert)
//   <layer>:fragment = a shader path, e.g. shaders/gradient.frag (or prefab:gradient.frag)
//   <layer>:<name>   = <value>                a parameter default
//
// Two namespaces, no shadowing. A preset is "prefab:<name>", an embedded
// built-in, or the path of a .conf file with its extension, e.g.
// "presets/subtle.conf" (cfg::resolveShaderPath(): relative ones against
// <hyprtail root>, cfg::hyprtailRoot(): $XDG_CONFIG_HOME/hypr/hyprtail,
// fallback ~/.config/hypr/hyprtail). A bare "<name>" is an error.
//
// Shader stages are paths relative to the same root, whichever kind of
// preset names them: in an embedded preset they are looked up in the
// embedded shader table by that string and never touch the disk; in a file
// preset they are files, and a missing one is a warning (load() below).
// "prefab:<name>" in a file preset is the embedded "shaders/<name>".
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
    // and up to 4 SLayerSpecs with vertex/fragment already resolved to an
    // embedded shader::builtin() key ("shaders/ribbon.vert") or an absolute
    // path.
    struct SResolved {
        std::string                        name, description;
        std::vector<SLayerSpec>            layers;
        std::string                        sourceKind{source::DEFAULT_KIND};
        std::map<std::string, std::string> sourceDefaults; // source settings from the manifest

        bool                               operator==(const SResolved&) const = default;
    };

    // Resolves `name`: "prefab:<name>" or the path of a .conf file (see
    // above). Any failure (unknown prefab, a bare name or other value that
    // isn't a .conf path, no such file, parse error, an unrecognized shader
    // reference, an unresolvable path) is reported (diag, "trail:<name>")
    // and falls back to the embedded "prefab:subtle" manifest, which is
    // guaranteed to parse -- it ships with the plugin.
    //
    // One exception: a file preset that parses but names shader files that
    // aren't on disk is a warning, not an error. With `haveActive` (a trail
    // is already showing) it returns nullopt and the caller keeps what it
    // has; without, it falls back like the others.
    std::optional<SResolved> load(const std::string& name, bool haveActive);
}
