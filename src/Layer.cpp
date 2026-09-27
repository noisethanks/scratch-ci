#include "Layer.hpp"

#include <algorithm>
#include <format>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;

namespace hyprtail {
    const std::vector<SLayerSpec>& classicPreset() {
        static const std::vector<SLayerSpec> p{
            {.name = "trail", .vertBuiltin = "classic/ribbon.vert", .fragBuiltin = "classic/ribbon.frag", .defaults = {}},
            {.name = "idle", .vertBuiltin = "classic/ring.vert", .fragBuiltin = "classic/ring.frag", .defaults = {{"enabled", "false"}}},
        };
        return p;
    }

    CLayer::CLayer(const SLayerSpec& spec) : slot(spec.name, spec.vertBuiltin, spec.fragBuiltin), m_name(spec.name), m_overrides(spec.defaults) {}

    const std::string& CLayer::name() const {
        return m_name;
    }

    void CLayer::setOverrides(std::map<std::string, std::string> overrides) {
        if (overrides == m_overrides)
            return;
        m_overrides = std::move(overrides);
        ++m_overridesVersion;
    }

    bool CLayer::resolved() const {
        return slot.shader() && res.generation == slot.generation();
    }

    bool CLayer::enabledSetting() const {
        const auto it = m_overrides.find("enabled");
        if (it == m_overrides.end())
            return true;
        const auto v = params::parseValue(params::eType::BOOL, it->second);
        return !v || v->x != 0.0;
    }

    shader::eTopology CLayer::topology() const {
        return slot.info().topology;
    }

    void CLayer::resolve() {
        if (!slot.shader() || (res.generation == slot.generation() && res.overrides == m_overridesVersion))
            return;

        const auto&  info = slot.info();
        const auto   key  = "params:" + m_name;
        std::string  problems;

        // Declarations: reserved first, then the program's own.
        std::vector<std::pair<params::SDecl, params::SValue>> all;
        for (const auto& r : shader::reservedParams()) {
            auto decl = r.decl;
            if (decl.name == "draw_when_cursor_hidden" && info.topology == shader::eTopology::QUAD)
                decl.def.x = 0.0; // an idle marker defeats a cursor hide
            all.emplace_back(decl, decl.def);
        }
        for (const auto& d : info.params)
            all.emplace_back(d, d.def);

        for (const auto& [name, text] : m_overrides) {
            const auto it = std::ranges::find_if(all, [&](const auto& e) { return e.first.name == name; });
            if (it == all.end())
                continue;
            auto v = params::parseValue(it->first.type, text);
            if (v)
                if (auto r = params::checkRange(it->first, *v); !r)
                    v = std::unexpected(r.error());
            if (!v) {
                problems += std::format("\n  {}: {}; using {}", name, v.error(), params::format(it->second));
                continue;
            }
            it->second = *v;
        }

        const auto lookup = [&all](std::string_view n) -> std::optional<double> {
            const auto it = std::ranges::find_if(all, [&](const auto& e) { return e.first.name == n; });
            return it == all.end() ? std::nullopt : params::scalar(it->second);
        };
        const auto get = [&](std::string_view n) { return lookup(n).value_or(0.0); };

        SResolved r;
        r.generation     = slot.generation();
        r.overrides      = m_overridesVersion;
        r.enabled        = get("enabled") != 0.0;
        r.drawWhenHidden = get("draw_when_cursor_hidden") != 0.0;
        r.fadeMs         = get("fade_ms");
        r.startMs        = get("start_ms");
        r.durationMs     = get("duration_ms");

        for (const auto& pad : info.padding) {
            const auto v = pad.expr.eval(lookup);
            if (!v) {
                problems += std::format("\n  padding at {}: {}; ignoring it", pad.where, v.error());
                continue;
            }
            if (*v < 0.0 || *v > 4096.0)
                problems += std::format("\n  padding at {} = {} is outside 0..4096; clamped", pad.where, *v);
            r.paddingPx = std::max(r.paddingPx, static_cast<float>(std::clamp(*v, 0.0, 4096.0)));
        }

        for (size_t i = shader::reservedParams().size(); i < all.size(); ++i)
            r.values.push_back(all[i]);

        res = std::move(r);

        diag::resetKey(key);
        if (!problems.empty())
            diag::report(eSeverity::WARN, key, std::format("layer {}: parameter problems:{}", m_name, problems));
    }
}
