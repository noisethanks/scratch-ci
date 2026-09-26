#include "Status.hpp"

#include <format>

namespace hyprtail::status {
    namespace {
        std::string shaderText(const SSlotStatus& sh) {
            const auto origin = [](const std::string& o) { return o.empty() ? std::string{"built-in"} : o; };
            if (!sh.active)
                return std::format("none active (last compile: {}{})", sh.lastResult, sh.pending ? ", reload pending" : "");
            return std::format("vertex {}, fragment {} (last compile: {}{})", origin(sh.vertOrigin), origin(sh.fragOrigin), sh.lastResult,
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
            return std::format(R"({{"active": {}, "pending": {}, "vertex": "{}", "fragment": "{}", "lastResult": "{}"}})", b(sh.active), b(sh.pending), esc(sh.vertOrigin),
                               esc(sh.fragOrigin), esc(sh.lastResult));
        }

        std::string boxJson(const std::optional<CBox>& box) {
            if (!box)
                return "null";
            return std::format(R"({{"x": {:.1f}, "y": {:.1f}, "w": {:.1f}, "h": {:.1f}}})", box->x, box->y, box->w, box->h);
        }
    }

    std::string text(const SSnapshot& s) {
        std::string out = std::format("hyprtail {}\n", s.rev);
        out += std::format("  hyprland: running {}, built against {}{}\n", s.runningHash, s.builtHash, s.runningHash == s.builtHash ? "" : " (MISMATCH)");
        out += std::format("  hooks: cursor {}, warp {}\n", s.cursorHook ? "active" : "unavailable (trail draws above the cursor)",
                           s.warpHook ? "active" : "unavailable (warps always connect)");
        out += std::format("  renders: {}\n", s.renders);

        out += std::format("  trail: {}, {}/{} points, generation {}, pending break {}, warps {}, fade {:.0f} ms\n", s.trail.disabled ? "DISABLED (see errors.log)" : "on",
                           s.trail.nodes, s.trail.capacity, s.trail.generation, s.trail.pendingBreak ? "yes" : "no", s.trail.interpolateWarps ? "connect" : "break",
                           s.trail.fadeMs);
        out += std::format("    shader: {}\n", shaderText(s.trail.shader));

        const char* idleState = !s.idle.enabled ? "off" : s.idle.disabled ? "DISABLED (see errors.log)" : s.idle.showing ? "showing" : "waiting";
        out += std::format("  idle: {}, pointer still for {:.0f} ms\n", idleState, s.idle.stillMs);
        out += std::format("    shader: {}\n", shaderText(s.idle.shader));

        out += "  monitors:\n";
        if (s.monitors.empty())
            out += "    none\n";
        for (const auto& m : s.monitors) {
            out += std::format("    {}: {} renders (lifecycle via hook {}, fallback {}), {} without damage\n", m.name, m.renders, m.hookRuns, m.fallbackRuns, m.emptySkips);
            out += std::format("      trail: {} draws, {}\n", m.trailDraws, boxText(m.trailBox));
            out += std::format("      idle: {} draws, {}\n", m.idleDraws, boxText(m.idleBox));
        }

        out += std::format("  reports this load: {} errors, {} warnings{}\n", s.diag.errors, s.diag.warnings, s.diag.batchOpen ? " (batch open)" : "");
        out += std::format("  errors.log: {}", s.errorFile.empty() ? "unavailable" : s.errorFile);
        return out;
    }

    std::string json(const SSnapshot& s) {
        std::string out = "{";
        out += std::format(R"("rev": "{}", "builtHash": "{}", "runningHash": "{}", )", esc(s.rev), esc(s.builtHash), esc(s.runningHash));
        out += std::format(R"("hooks": {{"cursor": {}, "warp": {}}}, "renders": {}, )", b(s.cursorHook), b(s.warpHook), s.renders);
        out += std::format(R"("trail": {{"disabled": {}, "nodes": {}, "capacity": {}, "generation": {}, "pendingBreak": {}, "interpolateWarps": {}, "fadeMs": {:.1f}, "shader": {}}}, )",
                           b(s.trail.disabled), s.trail.nodes, s.trail.capacity, s.trail.generation, b(s.trail.pendingBreak), b(s.trail.interpolateWarps), s.trail.fadeMs,
                           shaderJson(s.trail.shader));
        out += std::format(R"("idle": {{"enabled": {}, "disabled": {}, "showing": {}, "stillMs": {:.1f}, "shader": {}}}, )", b(s.idle.enabled), b(s.idle.disabled),
                           b(s.idle.showing), s.idle.stillMs, shaderJson(s.idle.shader));

        out += R"("monitors": [)";
        for (size_t i = 0; i < s.monitors.size(); ++i) {
            const auto& m = s.monitors[i];
            out += std::format(
                R"({}{{"name": "{}", "renders": {}, "hookRuns": {}, "fallbackRuns": {}, "trailDraws": {}, "idleDraws": {}, "emptyDamageSkips": {}, "trailBox": {}, "idleBox": {}}})",
                i ? ", " : "", esc(m.name), m.renders, m.hookRuns, m.fallbackRuns, m.trailDraws, m.idleDraws, m.emptySkips, boxJson(m.trailBox), boxJson(m.idleBox));
        }
        out += "], ";

        out += std::format(R"("reports": {{"errors": {}, "warnings": {}, "batchOpen": {}}}, "errorFile": "{}")", s.diag.errors, s.diag.warnings, b(s.diag.batchOpen),
                           esc(s.errorFile));
        out += "}";
        return out;
    }
}
