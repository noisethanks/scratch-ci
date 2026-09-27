#pragma once

#include <map>
#include <string>
#include <vector>

#include "Params.hpp"
#include "RenderUtil.hpp"
#include "ShaderSlot.hpp"

// One layer of a preset (SPEC §13.1): a shader program (geometry + shading)
// over the preset's shared source, with its parameter values and its own
// per-monitor damage lifecycle.
namespace hyprtail {
    struct SLayerSpec {
        std::string                        name;
        std::string                        vertBuiltin, fragBuiltin; // shader::builtin() names
        std::map<std::string, std::string> defaults;                 // param name -> value text
    };

    // The built-in "classic" preset: today's look. Layer "trail" (path
    // ribbon) and layer "idle" (quad ring, off by default).
    const std::vector<SLayerSpec>& classicPreset();

    // Values for the active program, recomputed when the program or the
    // overrides change.
    struct SResolved {
        uint64_t                                            generation = UINT64_MAX; // slot generation resolved for
        uint64_t                                            overrides  = UINT64_MAX; // overrides version resolved for
        std::vector<std::pair<params::SDecl, params::SValue>> values;                // the program's params

        // Reserved lifecycle parameters.
        bool   enabled        = true;
        bool   drawWhenHidden = true;
        double fadeMs         = 500.0;
        double startMs        = 500.0;
        double durationMs     = 1500.0;

        // Largest padding expression of the program, px (without damage_padding).
        float  paddingPx = 0.F;
    };

    class CLayer {
      public:
        explicit CLayer(const SLayerSpec& spec);

        const std::string& name() const;

        // Preset defaults merged with config values (config wins), as value
        // text by parameter name. Unknown names are ignored, silently: the
        // classic preset's config mapping sets names a user shader may not
        // declare.
        void               setOverrides(std::map<std::string, std::string> overrides);

        // Entries of the `params` config string (SPEC §13.5) addressed to
        // this layer, value text by parameter name. Applied on top of
        // setOverrides(); unlike it, an unknown name here is reported
        // (params:<layer>), not silently ignored.
        void               setParamOverrides(std::map<std::string, std::string> overrides);

        // Recompute res if the program or the overrides changed. Reports bad
        // values (params:<layer>) and keeps the declared default for them.
        void               resolve();
        bool               resolved() const; // res matches the active program

        // "enabled" from the overrides alone, usable before any program is
        // active (a disabled layer isn't compiled).
        bool               enabledSetting() const;

        shader::eTopology  topology() const;

        CShaderSlot        slot;
        CMonitorDamage     damage;
        SResolved          res;

        // Set after an unrecoverable failure (built-in shader, GL,
        // exception): the layer clears its last box once and stays off
        // until the plugin is reloaded.
        bool               disabled = false;

      private:
        std::string                        m_name;
        std::map<std::string, std::string> m_overrides;
        std::map<std::string, std::string> m_paramOverrides;
        uint64_t                           m_overridesVersion = 0; // bumped on either map changing
    };
}
