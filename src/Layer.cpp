#include "Layer.hpp"

#include <algorithm>
#include <format>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;

namespace hyprtail {
    CLayer::CLayer(const SLayerSpec& spec) : slot(spec.name, spec.vertBuiltin, spec.fragBuiltin), m_name(spec.name), m_overrides(spec.defaults) {}

    const std::string& CLayer::name() const {
        return m_name;
    }

    void CLayer::setParamOverrides(std::map<std::string, std::string> overrides) {
        if (overrides == m_paramOverrides)
            return;
        m_paramOverrides = std::move(overrides);
        ++m_overridesVersion;
    }

    bool CLayer::resolved() const {
        return slot.shader() && res.generation == slot.generation();
    }

    bool CLayer::enabledSetting() const {
        // m_paramOverrides (the `params` string) takes precedence over
        // m_overrides (the preset/config mapping), same as resolve().
        const auto pick = [](const std::map<std::string, std::string>& m) -> std::optional<std::string> {
            const auto it = m.find("enabled");
            return it == m.end() ? std::nullopt : std::optional{it->second};
        };
        const auto text = pick(m_paramOverrides).or_else([&] { return pick(m_overrides); });
        if (!text)
            return true;
        const auto v = params::parseValue(params::eType::BOOL, *text);
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

        // m_overrides (the active preset's own per-layer defaults) first,
        // then m_paramOverrides (the `params` string) on top, so params
        // wins. Both report an unknown name (SPEC §13.5/§13.7 "is an
        // error"): a preset manifest naming a param its own shader doesn't
        // declare is an authoring mistake worth surfacing, not the silent
        // best-effort mapping this used to be before presets were real (see
        // NOTES "Phase 4").
        const auto applyOverrides = [&](const std::map<std::string, std::string>& overrides) {
            for (const auto& [name, text] : overrides) {
                const auto it = std::ranges::find_if(all, [&](const auto& e) { return e.first.name == name; });
                if (it == all.end()) {
                    problems += std::format("\n  {}: not a parameter of this layer; ignoring", name);
                    continue;
                }
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
        };
        applyOverrides(m_overrides);
        applyOverrides(m_paramOverrides);

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

        if (info.topology == shader::eTopology::INSTANCED) {
            // A param K was range-checked (1..64) when the program was put
            // together and again for every value set; the clamp only keeps a
            // bad draw from ever sizing a buffer.
            const double k = info.instances.param.empty() ? info.instances.literal : get(info.instances.param);
            r.instances    = std::clamp(static_cast<int>(k), 1, shader::MAX_INSTANCES);
        }

        for (size_t i = shader::reservedParams().size(); i < all.size(); ++i)
            r.values.push_back(all[i]);

        res = std::move(r);

        diag::resetKey(key);
        if (!problems.empty())
            diag::report(eSeverity::WARN, key, std::format("layer {}: parameter problems:{}", m_name, problems));
    }
}
