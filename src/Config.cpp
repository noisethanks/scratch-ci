#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <sstream>
#include <string_view>

#include <config/ConfigManager.hpp>
#include <config/values/types/BoolValue.hpp>
#include <config/values/types/FloatValue.hpp>
#include <config/values/types/IntValue.hpp>
#include <config/values/types/StringValue.hpp>
#include <config/values/types/Vec2Value.hpp>

#include "ConfigParse.hpp"
#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;
using namespace Config::Values;

namespace hyprtail::cfg {
    const char* warpModeName(eWarpMode m) {
        switch (m) {
            case eWarpMode::LINE: return "line";
            case eWarpMode::CURVE: return "curve";
            default: return "break";
        }
    }

    namespace {
        // Names are stored as raw const char* by IValue: literals only.
        // Min/max are also enforced by Hyprland when parsing, with its own
        // config error; read() re-checks so a bad value can never reach us.
        struct SRegistered {
            SP<CFloatValue>                 minSpacing, damagePadding;
            SP<CIntValue>                   capacity;
            SP<CStringValue>                warp;
            SP<CStringValue>                trail;
            std::array<SP<CStringValue>, 4> layerVertex, layerFragment;
            SP<CStringValue>                params;
            SP<CStringValue>                screenshare;
            SP<CStringValue>                emitFrom;
            SP<CVec2Value>                  emitOffset;
        };

        SRegistered& reg() {
            static SRegistered r;
            return r;
        }

        const SValues DEFAULTS{};

        bool          add(HANDLE handle, const SP<IValue>& v) {
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

        // One of `allowed`, else report and keep `fallback`.
        std::string checkEnum(const SP<CStringValue>& v, std::initializer_list<std::string_view> allowed, const std::string& fallback) {
            if (!v)
                return fallback;
            auto val = v->value();
            if (std::ranges::find(allowed, std::string_view{val}) != allowed.end()) {
                diag::resetKey(std::format("config:{}", v->name()));
                return val;
            }
            std::string list;
            for (const auto& a : allowed)
                list += (list.empty() ? "" : ", ") + std::string{a};
            diag::report(eSeverity::WARN, std::format("config:{}", v->name()), std::format(R"({} = "{}" isn't one of {}; keeping "{}")", v->name(), val, list, fallback));
            return fallback;
        }

    }

    bool registerValues(HANDLE handle) {
        auto& r = reg();

        // Ranges: see SPEC section 9.
        r.capacity =
            makeShared<CIntValue>("plugin:hyprtail:capacity", "max number of trail points kept", sc<Config::INTEGER>(DEFAULTS.capacity), SIntValueOptions{.min = 2, .max = 4096});
        r.minSpacing    = makeShared<CFloatValue>("plugin:hyprtail:min_spacing", "min pointer travel between trail points, logical px", DEFAULTS.minSpacingPx,
                                                  SFloatValueOptions{.min = 0.F, .max = 256.F});
        r.warp          = makeShared<CStringValue>("plugin:hyprtail:warp", R"(how the trail crosses a pointer warp: "break" (default), "line", or "curve" (SPEC section 13.10))",
                                                   warpModeName(DEFAULTS.warp));
        r.damagePadding = makeShared<CFloatValue>("plugin:hyprtail:damage_padding", "extra damage padding on top of the stock extent and shader-declared padding, px",
                                                  DEFAULTS.damagePaddingPx, SFloatValueOptions{.min = 0.F, .max = 4096.F});
        r.trail    = makeShared<CStringValue>("plugin:hyprtail:trail",
                                              "which trail to use (SPEC section 13.7): \"prefab:<name>\" (built-in: jitter, vivid, comet, embers, spring, ink, mosaic, snake, helix, tether, thread) "
                                              "or the path of a .conf file relative to <hyprtail root>, e.g. \"presets/ink.conf\"",
                                              DEFAULTS.trail.c_str());
        r.emitFrom = makeShared<CStringValue>("plugin:hyprtail:emit_from",
                                              "where on the cursor image trail points are emitted from: \"hotspot\" (default), or a normalized \"x y\" position in "
                                              "the cursor image box, each in 0..1 (0 0 = top-left, 0.5 0.5 = center) (SPEC section 13.9)",
                                              "hotspot");
        r.emitOffset =
            makeShared<CVec2Value>("plugin:hyprtail:emit_offset", "fixed pixel offset added after emit_from, logical px, each component within +-128 (SPEC section 13.9)",
                                   Config::VEC2{sc<float>(DEFAULTS.emitOffsetPx.x), sc<float>(DEFAULTS.emitOffsetPx.y)});
        // No SVec2ValueOptions::validator: at the pin neither provider applies
        // a CVec2Value validator (LuaConfigUtils.cpp:41-42, legacy
        // ConfigManager.cpp:501-502 take only defaultVal()), so the bound is
        // enforced in read() alone.

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
        r.params      = makeShared<CStringValue>("plugin:hyprtail:params", R"(per-layer parameter overrides: "<layer>:<name>=<value> ..." (SPEC section 13.5))", "");
        r.screenshare = makeShared<CStringValue>("plugin:hyprtail:screenshare", R"("exclude" (default) to keep the trail out of monitor/region captures and mirrors, or "include")",
                                                 DEFAULTS.screenshare.c_str());

        bool ok = true;
        for (const SP<IValue>& v :
             std::initializer_list<SP<IValue>>{r.capacity, r.minSpacing, r.warp, r.damagePadding, r.trail, r.params, r.screenshare, r.emitFrom, r.emitOffset})
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

        {
            const auto text = checkEnum(r.warp, {"break", "line", "curve"}, warpModeName(previous.warp));
            v.warp          = text == "line" ? eWarpMode::LINE : text == "curve" ? eWarpMode::CURVE : eWarpMode::BREAK;
        }
        if (r.trail)
            v.trail = r.trail->value();

        if (r.emitFrom) {
            const auto  text = r.emitFrom->value();
            const char* key  = "config:plugin:hyprtail:emit_from";
            if (text == "hotspot") {
                v.emitFromNorm = std::nullopt;
                diag::resetKey(key);
            } else {
                const auto parsed = parseTwoFloats(text);
                const auto kept   = previous.emitFromNorm ? std::format("\"{} {}\"", previous.emitFromNorm->x, previous.emitFromNorm->y) : std::string{"\"hotspot\""};
                if (parsed && emitFromInRange(*parsed)) {
                    v.emitFromNorm = Vector2D{parsed->x, parsed->y};
                    diag::resetKey(key);
                } else {
                    diag::report(
                        eSeverity::WARN, key,
                        std::format("plugin:hyprtail:emit_from = \"{}\" {}; keeping {}", text, parsed ? "has a component outside 0..1" : R"(isn't "hotspot" or "x y")", kept));
                    v.emitFromNorm = previous.emitFromNorm;
                }
            }
        }
        if (r.emitOffset) {
            const auto  vec  = r.emitOffset->value();
            const char* key  = "config:plugin:hyprtail:emit_offset";
            const auto  next = resolveEmitOffset(vec.x, vec.y, SPair{previous.emitOffsetPx.x, previous.emitOffsetPx.y});
            v.emitOffsetPx   = {next.x, next.y};
            if (emitOffsetInRange(vec.x, vec.y))
                diag::resetKey(key);
            else
                diag::report(eSeverity::WARN, key,
                             std::format("plugin:hyprtail:emit_offset = {} {} is outside +-{} on an axis; keeping {} {}", vec.x, vec.y, EMIT_OFFSET_MAX_PX, previous.emitOffsetPx.x,
                                         previous.emitOffsetPx.y));
        }

        for (size_t i = 0; i < 4; ++i) {
            if (r.layerVertex[i])
                v.layerVertex[i] = r.layerVertex[i]->value();
            if (r.layerFragment[i])
                v.layerFragment[i] = r.layerFragment[i]->value();
        }
        if (r.params)
            v.params = r.params->value();

        v.screenshare = checkEnum(r.screenshare, {"exclude", "include"}, previous.screenshare);

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
        return hyprtailRoot() / p;
    }

    std::filesystem::path hyprtailRoot() {
        const char* xdg  = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && xdg[0] == '/')
            return std::filesystem::path{xdg} / "hypr" / "hyprtail";
        if (home && home[0] == '/')
            return std::filesystem::path{home} / ".config" / "hypr" / "hyprtail";
        return {};
    }
}
