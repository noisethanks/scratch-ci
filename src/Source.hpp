#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Params.hpp"
#include "TrailBuffer.hpp"

// Trail sources by name (SPEC §13.7): the values a preset's `source` key can
// take, the settings each one declares (`source:<name>` keys and `params`
// entries, same value syntax and range checks as shader parameters), and the
// factory. No Hyprland headers.
//
//   pointer   real pointer history (CTrailRing), no settings
//   spring    a chain of points chasing each other (CSpringChainSource):
//             mass, stiffness, damping, age_step_ms
namespace hyprtail::source {
    // The source of a preset that names none.
    constexpr std::string_view DEFAULT_KIND = "pointer";

    // The layer-key prefix that addresses the source in a manifest and in
    // the `params` string: "source:<name>". Not usable as a layer name.
    constexpr std::string_view KEY_PREFIX = "source";

    bool                              known(std::string_view kind);
    std::string                       kindList(); // "pointer, spring"

    // The settings `kind` declares; empty for an unknown kind.
    const std::vector<params::SDecl>& decls(std::string_view kind);

    // A new source of `kind` (which must be known) holding `capacity` nodes.
    std::unique_ptr<ISource>          make(std::string_view kind, size_t capacity, uint64_t seedBase);

    // Declared defaults, then the preset's `defaults`, then the `params`
    // string's `overrides`, each by name, as numbers for ISource::configure.
    // Unknown names and bad values are described in `problems` ("\n  name:
    // ...", see params::applyOverrides) and leave the earlier value.
    std::map<std::string, double>     resolve(std::string_view kind, const std::map<std::string, std::string>& defaults, const std::map<std::string, std::string>& overrides,
                                              std::string& problems);
}
