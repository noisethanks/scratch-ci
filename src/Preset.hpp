#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Layer.hpp"

// Preset manifests (SPEC §13.7): preset.conf, plain "key = value" lines, '#'
// to end-of-line comments, blank lines ignored. Layer keys are prefixed
// "<layer>:", as in "plugin:hyprtail:...".
//
//   contract    = 2                          required, exactly once
//   description = ...                        optional, at most once
//   layers      = <name>[, <name>...]         required, 1-4, no duplicates
//
//   <layer>:vertex   = <built-in name or path>
//   <layer>:fragment = <built-in name or path>
//   <layer>:<name>   = <value>                a parameter default
//
// Built-in presets are embedded; user presets live in
// $XDG_CONFIG_HOME/hypr/hyprtail/presets/<name>/ (fallback ~/.config/...)
// and shadow a built-in of the same name.
namespace hyprtail::preset {
    // Manifest grammar version, unrelated to shader::CONTRACT_VERSION (§5):
    // this one just lets a future breaking change to preset.conf's own
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
        std::string                                                description;
        std::vector<std::string>                                   layers; // draw order, first = bottom
        std::map<std::string, std::map<std::string, std::string>>  layerKeys; // layer name -> (key -> raw value text, incl. "vertex"/"fragment")

        bool operator==(const SManifest&) const = default;
    };

    std::expected<SManifest, std::string> parse(std::string_view text);

    // A resolved preset, ready for CLayer construction: name, description,
    // and up to 4 SLayerSpecs with vertex/fragment already resolved to a
    // built-in shader::builtin() name or an absolute path.
    struct SResolved {
        std::string             name, description;
        std::vector<SLayerSpec> layers;

        bool operator==(const SResolved&) const = default;
    };

    // Resolves `name`: a user preset directory shadows a built-in of the
    // same name. Any failure (not found, parse error, an unrecognized
    // built-in shader name in a built-in preset, an unresolvable path in a
    // user preset) is reported (diag, "preset:<name>") and falls back to
    // the embedded "subtle" manifest, which is guaranteed to parse -- it
    // ships with the plugin.
    SResolved load(const std::string& name);
}
