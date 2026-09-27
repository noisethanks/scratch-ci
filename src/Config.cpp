#include "Config.hpp"

#include <cmath>
#include <cstdlib>
#include <format>

#include <config/ConfigManager.hpp>
#include <config/values/types/BoolValue.hpp>
#include <config/values/types/FloatValue.hpp>
#include <config/values/types/IntValue.hpp>
#include <config/values/types/StringValue.hpp>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;
using namespace Config::Values;

namespace hyprtail::cfg {
    namespace {
        // Names are stored as raw const char* by IValue: literals only.
        // Min/max are also enforced by Hyprland when parsing, with its own
        // config error; read() re-checks so a bad value can never reach us.
        struct SRegistered {
            SP<CFloatValue>                 minSpacing, damagePadding;
            SP<CIntValue>                   capacity;
            SP<CBoolValue>                  interpolateWarps;
            SP<CStringValue>                preset;
            std::array<SP<CStringValue>, 4> layerVertex, layerFragment;
            SP<CStringValue>                params;
        };

        SRegistered& reg() {
            static SRegistered r;
            return r;
        }

        const SValues DEFAULTS{};

        bool add(HANDLE handle, const SP<IValue>& v) {
            if (HyprlandAPI::addConfigValueV2(handle, v))
                return true;
            diag::report(eSeverity::WARN, std::format("config:{}", v->name()), std::format("could not register config value {}; using its built-in default", v->name()));
            return false;
        }

        // Float in [lo, hi], finite, else report and keep `fallback`.
        float checkFloat(const SP<CFloatValue>& v, float lo, float hi, float fallback) {
            if (!v)
                return fallback;
            const float x = v->value();
            if (std::isfinite(x) && x >= lo && x <= hi) {
                diag::resetKey(std::format("config:{}", v->name()));
                return x;
            }
            diag::report(eSeverity::WARN, std::format("config:{}", v->name()), std::format("{} = {} is outside {}..{}; keeping {}", v->name(), x, lo, hi, fallback));
            return fallback;
        }
    }

    bool registerValues(HANDLE handle) {
        auto& r = reg();

        // Ranges: see SPEC section 9.
        r.capacity =
            makeShared<CIntValue>("plugin:hyprtail:capacity", "max number of trail points kept", sc<Config::INTEGER>(DEFAULTS.capacity), SIntValueOptions{.min = 2, .max = 4096});
        r.minSpacing = makeShared<CFloatValue>("plugin:hyprtail:min_spacing", "min pointer travel between trail points, logical px", DEFAULTS.minSpacingPx,
                                               SFloatValueOptions{.min = 0.F, .max = 256.F});
        r.interpolateWarps =
            makeShared<CBoolValue>("plugin:hyprtail:interpolate_warps", "connect the trail across pointer warps instead of breaking it", DEFAULTS.interpolateWarps);
        r.damagePadding = makeShared<CFloatValue>("plugin:hyprtail:damage_padding", "extra damage padding on top of the stock extent and shader-declared padding, px",
                                                  DEFAULTS.damagePaddingPx, SFloatValueOptions{.min = 0.F, .max = 4096.F});
        r.preset        = makeShared<CStringValue>("plugin:hyprtail:preset", "which preset to use (SPEC section 13.7); built-in: subtle, classic", DEFAULTS.preset.c_str());

        // Per-layer shader overrides (SPEC §13.7): static keys indexed by
        // position in the preset's layer list, capped at 4. Names must be
        // literals (see the SRegistered comment above), so these are
        // spelled out rather than built with std::format.
        static constexpr std::array<const char*, 4> LAYER_VERT_KEYS{"plugin:hyprtail:layer1_vertex", "plugin:hyprtail:layer2_vertex", "plugin:hyprtail:layer3_vertex",
                                                                     "plugin:hyprtail:layer4_vertex"};
        static constexpr std::array<const char*, 4> LAYER_FRAG_KEYS{"plugin:hyprtail:layer1_fragment", "plugin:hyprtail:layer2_fragment", "plugin:hyprtail:layer3_fragment",
                                                                     "plugin:hyprtail:layer4_fragment"};
        for (size_t i = 0; i < 4; ++i) {
            r.layerVertex[i]   = makeShared<CStringValue>(LAYER_VERT_KEYS[i], "vertex shader override for this layer of the preset, empty uses the preset's own shader", "");
            r.layerFragment[i] = makeShared<CStringValue>(LAYER_FRAG_KEYS[i], "fragment shader override for this layer of the preset, empty uses the preset's own shader", "");
        }
        r.params = makeShared<CStringValue>("plugin:hyprtail:params", R"(per-layer parameter overrides: "<layer>:<name>=<value> ..." (SPEC section 13.5))", "");

        bool ok = true;
        for (const SP<IValue>& v : std::initializer_list<SP<IValue>>{r.capacity, r.minSpacing, r.interpolateWarps, r.damagePadding, r.preset, r.params})
            ok = add(handle, v) && ok;
        for (size_t i = 0; i < 4; ++i) {
            ok = add(handle, r.layerVertex[i]) && ok;
            ok = add(handle, r.layerFragment[i]) && ok;
        }
        return ok;
    }

    void releaseValues() {
        reg() = {};
    }

    SValues read(const SValues& previous) {
        auto&   r = reg();
        SValues v = previous;

        v.minSpacingPx    = checkFloat(r.minSpacing, 0.F, 256.F, previous.minSpacingPx);
        v.damagePaddingPx = checkFloat(r.damagePadding, 0.F, 4096.F, previous.damagePaddingPx);

        if (r.capacity) {
            const auto c = r.capacity->value();
            if (c >= 2 && c <= 4096) {
                v.capacity = sc<size_t>(c);
                diag::resetKey("config:plugin:hyprtail:capacity");
            } else
                diag::report(eSeverity::WARN, "config:plugin:hyprtail:capacity", std::format("plugin:hyprtail:capacity = {} is outside 2..4096; keeping {}", c, previous.capacity));
        }

        if (r.interpolateWarps)
            v.interpolateWarps = r.interpolateWarps->value();
        if (r.preset)
            v.preset = r.preset->value();

        for (size_t i = 0; i < 4; ++i) {
            if (r.layerVertex[i])
                v.layerVertex[i] = r.layerVertex[i]->value();
            if (r.layerFragment[i])
                v.layerFragment[i] = r.layerFragment[i]->value();
        }
        if (r.params)
            v.params = r.params->value();

        return v;
    }

    std::filesystem::path resolveShaderPath(const std::string& configured) {
        if (configured.empty())
            return {};

        if (configured == "~" || configured.starts_with("~/")) {
            const char* home = std::getenv("HOME");
            if (home && home[0] == '/')
                return std::filesystem::path{home} / configured.substr(configured.size() > 1 ? 2 : 1);
            return configured;
        }

        std::filesystem::path p{configured};
        if (p.is_absolute())
            return p;

        // Directory of the config file actually in use (covers -c), else
        // $XDG_CONFIG_HOME/hypr, else ~/.config/hypr.
        std::filesystem::path base;
        if (Config::mgr()) {
            const std::filesystem::path main = Config::mgr()->getMainConfigPath();
            if (main.is_absolute())
                base = main.parent_path();
        }
        if (base.empty()) {
            const char* xdg  = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            if (xdg && xdg[0] == '/')
                base = std::filesystem::path{xdg} / "hypr";
            else if (home && home[0] == '/')
                base = std::filesystem::path{home} / ".config" / "hypr";
        }

        return base / p;
    }
}
