#include "Status.hpp"

#include <format>

namespace hyprtail::status {
    namespace {
        std::string shaderText(const SSlotStatus& sh) {
            const auto origin = [](const std::string& o) { return o.empty() ? std::string{"built-in"} : o; };
            if (!sh.active)
                return std::format("none active (last compile: {}{})", sh.lastResult, sh.pending ? ", reload pending" : "");
            return std::format("topology {}, vertex {}, fragment {} (last compile: {}{})", sh.topology, origin(sh.vertOrigin), origin(sh.fragOrigin), sh.lastResult,
                               sh.pending ? ", reload pending" : "");
        }

        std::string boxText(const std::optional<CBox>& b) {
            if (!b)
                return "never drawn";
            if (b->empty())
                return "idle";
            return std::format("drawing {:.0f}x{:.0f} at {:.0f},{:.0f}", b->w, b->h, b->x, b->y);
        }

        std::string esc(std::string_view in) {
            std::string out;
            out.reserve(in.size() + 2);
            for (const char c : in) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20)
                            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                        else
                            out += c;
                }
            }
            return out;
        }

        const char* b(bool v) {
            return v ? "true" : "false";
        }

        std::string shaderJson(const SSlotStatus& sh) {
            return std::format(R"({{"active": {}, "pending": {}, "topology": "{}", "vertex": "{}", "fragment": "{}", "lastResult": "{}"}})", b(sh.active), b(sh.pending),
                               esc(sh.topology), esc(sh.vertOrigin), esc(sh.fragOrigin), esc(sh.lastResult));
        }

        std::string boxJson(const std::optional<CBox>& box) {
            if (!box)
                return "null";
            return std::format(R"({{"x": {:.1f}, "y": {:.1f}, "w": {:.1f}, "h": {:.1f}}})", box->x, box->y, box->w, box->h);
        }

        // Active suppress conditions in words, comma-separated, empty if none.
        std::string suppressConditions(const SSnapshot& s) {
            std::vector<std::string_view> active;
            if (s.suppress.locked)
                active.push_back("session lock");
            if (s.suppress.constrained)
                active.push_back("pointer constraint");
            if (s.suppress.appRule)
                active.push_back("app rule");
            std::string out;
            for (size_t i = 0; i < active.size(); ++i)
                out += std::format("{}{}", i ? ", " : "", active[i]);
            return out;
        }
    }

    std::string text(const SSnapshot& s) {
        std::string out = std::format("hyprtail {}\n", s.rev);
        out += std::format("  hyprland: running {}, built against {}{}\n", s.runningHash, s.builtHash, s.runningHash == s.builtHash ? "" : " (MISMATCH)");
        out += std::format("  hooks: cursor {}, warp {}, capture {}\n", s.cursorHook ? "active" : "unavailable (trail draws above the cursor)",
                           s.warpHook ? "active" : "unavailable (warps always connect)",
                           s.captureHook ? "active" : "unavailable (screenshare exclude degrades to not drawing on monitors that need a copy)");
        out += std::format("  renders: {}\n", s.renders);

        out += std::format("  trail: {}\n", s.trail);
        out += std::format("  screenshare: {}\n", s.screenshare);
        out += std::format("  source: {}, {}/{} points{}, generation {}, pending break {}, warp {}, pointer still for {:.0f} ms{}\n", s.source.kind, s.source.nodes,
                           s.source.capacity, s.source.moving ? " (moving)" : "", s.source.generation, s.source.pendingBreak ? "yes" : "no", s.source.warpMode, s.source.stillMs,
                           s.source.gpuFailed ? ", NODE BUFFER FAILED (see errors.log)" : "");
        const auto conditions = suppressConditions(s);
        out += conditions.empty() ? "  suppressed: no\n" : std::format("  suppressed: yes ({})\n", conditions);
        out += std::format("  window under pointer: {}\n",
                           s.suppress.hoveredClass.empty() ? "none" : std::format(R"(class "{}", title "{}")", s.suppress.hoveredClass, s.suppress.hoveredTitle));

        for (const auto& l : s.layers) {
            const char* state = l.disabled ? "DISABLED (see errors.log)" : !l.enabled ? "off" : l.resolved ? "on" : "on, not compiled yet";
            out += std::format("  layer {}: {}\n", l.name, state);
            out += std::format("    shader: {}\n", shaderText(l.shader));
            if (l.resolved) {
                if (l.shader.topology == "quad")
                    out += std::format("    lifecycle: fade {:.0f} ms, start {:.0f} ms, duration {:.0f} ms, reach {:.1f} px\n", l.fadeMs, l.startMs, l.durationMs, l.extentPx);
                else
                    out += std::format("    lifecycle: fade {:.0f} ms, reach {:.1f} px\n", l.fadeMs, l.extentPx);
                std::string params;
                for (const auto& [name, value] : l.params)
                    params += std::format(" {}={}", name, value);
                out += std::format("    params:{}\n", params.empty() ? " none" : params);
            }
        }

        out += "  monitors:\n";
        if (s.monitors.empty())
            out += "    none\n";
        for (const auto& m : s.monitors) {
            out += std::format("    {}: {} renders (lifecycle via hook {}, fallback {}), {} with layers drawn, {} without damage{}\n", m.name, m.renders, m.hookRuns,
                               m.fallbackRuns, m.draws, m.emptySkips,
                               !m.needsCopyFB        ? "" :
                                   m.captureFallback ? ", needs a copy (mirrored/captured), capture hook fallback active: not drawing there" :
                                                       ", needs a copy (mirrored/captured), drawn via the capture hook");
            for (size_t i = 0; i < m.layerBoxes.size() && i < s.layers.size(); ++i)
                out += std::format("      {}: {}\n", s.layers[i].name, boxText(m.layerBoxes[i]));
        }

        out += std::format("  reports this load: {} errors, {} warnings{}\n", s.diag.errors, s.diag.warnings, s.diag.batchOpen ? " (batch open)" : "");
        out += std::format("  errors.log: {}", s.errorFile.empty() ? "unavailable" : s.errorFile);
        return out;
    }

    std::string json(const SSnapshot& s) {
        std::string out = "{";
        out += std::format(R"("rev": "{}", "builtHash": "{}", "runningHash": "{}", "trail": "{}", "screenshare": "{}", )", esc(s.rev), esc(s.builtHash), esc(s.runningHash),
                           esc(s.trail), esc(s.screenshare));
        out += std::format(R"("hooks": {{"cursor": {}, "warp": {}, "capture": {}}}, "renders": {}, )", b(s.cursorHook), b(s.warpHook), b(s.captureHook), s.renders);
        out += std::format(
            R"("source": {{"nodes": {}, "capacity": {}, "generation": {}, "pendingBreak": {}, "warp": "{}", "gpuFailed": {}, "stillMs": {:.1f}, "kind": "{}", "moving": {}}}, )",
            s.source.nodes, s.source.capacity, s.source.generation, b(s.source.pendingBreak), esc(s.source.warpMode), b(s.source.gpuFailed), s.source.stillMs, esc(s.source.kind),
            b(s.source.moving));
        out += std::format(R"("suppress": {{"locked": {}, "constrained": {}, "appRule": {}, "hoveredClass": "{}", "hoveredTitle": "{}"}}, )", b(s.suppress.locked),
                           b(s.suppress.constrained), b(s.suppress.appRule), esc(s.suppress.hoveredClass), esc(s.suppress.hoveredTitle));

        out += R"("layers": [)";
        for (size_t i = 0; i < s.layers.size(); ++i) {
            const auto& l = s.layers[i];
            std::string params;
            for (size_t j = 0; j < l.params.size(); ++j)
                params += std::format(R"({}"{}": "{}")", j ? ", " : "", esc(l.params[j].first), esc(l.params[j].second));
            out += std::format(
                R"({}{{"name": "{}", "enabled": {}, "disabled": {}, "resolved": {}, "fadeMs": {:.1f}, "startMs": {:.1f}, "durationMs": {:.1f}, "extentPx": {:.1f}, "params": {{{}}}, "shader": {}}})",
                i ? ", " : "", esc(l.name), b(l.enabled), b(l.disabled), b(l.resolved), l.fadeMs, l.startMs, l.durationMs, l.extentPx, params, shaderJson(l.shader));
        }
        out += "], ";

        out += R"("monitors": [)";
        for (size_t i = 0; i < s.monitors.size(); ++i) {
            const auto& m = s.monitors[i];
            std::string boxes;
            for (size_t j = 0; j < m.layerBoxes.size(); ++j)
                boxes += std::format("{}{}", j ? ", " : "", boxJson(m.layerBoxes[j]));
            out += std::format(
                R"({}{{"name": "{}", "renders": {}, "hookRuns": {}, "fallbackRuns": {}, "draws": {}, "emptyDamageSkips": {}, "needsCopyFB": {}, "captureFallback": {}, "layerBoxes": [{}]}})",
                i ? ", " : "", esc(m.name), m.renders, m.hookRuns, m.fallbackRuns, m.draws, m.emptySkips, b(m.needsCopyFB), b(m.captureFallback), boxes);
        }
        out += "], ";

        out += std::format(R"("reports": {{"errors": {}, "warnings": {}, "batchOpen": {}}}, "errorFile": "{}")", s.diag.errors, s.diag.warnings, b(s.diag.batchOpen),
                           esc(s.errorFile));
        out += "}";
        return out;
    }
}
