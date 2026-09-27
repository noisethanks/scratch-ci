#include "Preset.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "Diagnostics.hpp"
#include "ShaderSource.hpp"

using hyprtail::diag::eSeverity;

namespace hyprtail::preset {
    namespace {
        std::string_view trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        // "a, b ,c" -> ["a", "b", "c"], each trimmed, empty pieces dropped.
        std::vector<std::string> splitCommaTrim(std::string_view s) {
            std::vector<std::string> out;
            size_t                   start = 0;
            while (start <= s.size()) {
                const auto comma = s.find(',', start);
                const auto piece = trim(s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
                if (!piece.empty())
                    out.emplace_back(piece);
                if (comma == std::string_view::npos)
                    break;
                start = comma + 1;
            }
            return out;
        }
    }

    std::expected<SManifest, std::string> parse(std::string_view text) {
        struct SRaw {
            std::optional<std::string>              contract, description;
            std::optional<std::vector<std::string>> layers;
        } raw;
        std::map<std::string, std::map<std::string, std::string>> layerKeys;

        std::istringstream in{std::string{text}};
        std::string        rawLine;
        int                lineNo = 0;
        while (std::getline(in, rawLine)) {
            ++lineNo;
            const auto where = std::format("preset.conf:{}", lineNo);

            std::string_view line = rawLine;
            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);
            line = trim(line);
            if (line.empty())
                continue;

            const auto eq = line.find('=');
            if (eq == std::string_view::npos)
                return std::unexpected(std::format("{}: malformed line, expected \"key = value\": \"{}\"", where, line));
            const auto key   = trim(line.substr(0, eq));
            const auto value = trim(line.substr(eq + 1));
            if (key.empty())
                return std::unexpected(std::format("{}: empty key", where));

            const auto colon = key.find(':');
            if (colon == std::string_view::npos) {
                if (key == "contract") {
                    if (raw.contract)
                        return std::unexpected(std::format("{}: duplicate \"contract\"", where));
                    raw.contract = std::string{value};
                } else if (key == "description") {
                    if (raw.description)
                        return std::unexpected(std::format("{}: duplicate \"description\"", where));
                    raw.description = std::string{value};
                } else if (key == "layers") {
                    if (raw.layers)
                        return std::unexpected(std::format("{}: duplicate \"layers\"", where));
                    raw.layers = splitCommaTrim(value);
                } else
                    return std::unexpected(std::format("{}: unknown key \"{}\" (contract, description, layers, or \"<layer>:<name>\")", where, key));
                continue;
            }

            const auto layer = trim(key.substr(0, colon));
            const auto name  = trim(key.substr(colon + 1));
            if (layer.empty() || name.empty())
                return std::unexpected(std::format("{}: malformed layer key \"{}\"", where, key));
            auto& entry = layerKeys[std::string{layer}];
            if (entry.contains(std::string{name}))
                return std::unexpected(std::format("{}: duplicate \"{}\"", where, key));
            entry[std::string{name}] = std::string{value};
        }

        if (!raw.contract)
            return std::unexpected("missing \"contract = 2\"");
        if (*raw.contract != std::to_string(CONTRACT_VERSION))
            return std::unexpected(std::format("this hyprtail implements preset contract {}, the manifest declares contract {}", CONTRACT_VERSION, *raw.contract));

        if (!raw.layers || raw.layers->empty())
            return std::unexpected("missing or empty \"layers\"");
        if (raw.layers->size() > 4)
            return std::unexpected(std::format("\"layers\" lists {} layers; at most 4", raw.layers->size()));
        {
            std::set<std::string> seen;
            for (const auto& l : *raw.layers)
                if (!seen.insert(l).second)
                    return std::unexpected(std::format("\"layers\" lists \"{}\" twice", l));
        }
        for (const auto& [layer, keys] : layerKeys) {
            if (std::ranges::find(*raw.layers, layer) == raw.layers->end())
                return std::unexpected(std::format("\"{}:...\" keys given, but \"{}\" isn't in \"layers\"", layer, layer));
        }

        return SManifest{.description = raw.description.value_or(""), .layers = std::move(*raw.layers), .layerKeys = std::move(layerKeys)};
    }

    namespace {
        constexpr unsigned char SUBTLE_CONF[] = {
#embed "../presets/subtle/preset.conf"
        };
        constexpr unsigned char CLASSIC_CONF[] = {
#embed "../presets/classic/preset.conf"
        };

        template <size_t N>
        constexpr std::string_view view(const unsigned char (&data)[N]) {
            return {reinterpret_cast<const char*>(data), N};
        }

        std::string_view builtinManifest(std::string_view name) {
            static const std::map<std::string, std::string_view, std::less<>> m{
                {"subtle", view(SUBTLE_CONF)},
                {"classic", view(CLASSIC_CONF)},
            };
            const auto it = m.find(name);
            return it == m.end() ? std::string_view{} : it->second;
        }

        std::filesystem::path expandHome(const std::string& p) {
            if (p == "~" || p.starts_with("~/")) {
                const char* home = std::getenv("HOME");
                if (home && home[0] == '/')
                    return std::filesystem::path{home} / p.substr(p.size() > 1 ? 2 : 1);
            }
            return p;
        }

        // $XDG_CONFIG_HOME/hypr/hyprtail/presets, fallback ~/.config/hypr/....
        std::filesystem::path presetsBaseDir() {
            const char*           xdg  = std::getenv("XDG_CONFIG_HOME");
            const char*           home = std::getenv("HOME");
            std::filesystem::path base;
            if (xdg && xdg[0] == '/')
                base = std::filesystem::path{xdg} / "hypr";
            else if (home && home[0] == '/')
                base = std::filesystem::path{home} / ".config" / "hypr";
            else
                return {};
            return base / "hyprtail" / "presets";
        }

        std::expected<std::string, std::string> readFile(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::unexpected(std::format("can't open {}", path.string()));
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Resolves one shader stage of one layer: a recognized shader::builtin()
        // name, or (user presets only, presetDir non-empty) a path relative to
        // the preset's own directory. Always returns a safe built-in identity
        // (`safeBuiltin`) alongside a path override, so CShaderSlot's
        // constructor -- which unconditionally looks up the built-in text --
        // never sees an unresolvable name (a bad built-in reference is instead
        // caught structurally, right here, before any CLayer/CShaderSlot exists).
        std::expected<std::pair<std::string, std::string>, std::string> resolveStage(const std::string& layerName, const std::map<std::string, std::string>& keys,
                                                                                     const std::filesystem::path& presetDir, const char* stageKey, const char* safeBuiltin) {
            const auto it = keys.find(stageKey);
            if (it == keys.end())
                return std::unexpected(std::format("layer \"{}\" needs \"{}:{}\"", layerName, layerName, stageKey));
            const std::string& value = it->second;
            if (!shader::builtin(value).empty())
                return std::pair<std::string, std::string>{value, ""};
            if (presetDir.empty())
                return std::unexpected(std::format("layer \"{}\": \"{}\" isn't a recognized built-in shader", layerName, value));

            std::filesystem::path p = expandHome(value);
            if (p.is_relative())
                p = presetDir / p;
            std::error_code ec;
            if (const auto canon = std::filesystem::weakly_canonical(p, ec); !ec)
                p = canon;
            return std::pair<std::string, std::string>{safeBuiltin, p.string()};
        }

        std::expected<SLayerSpec, std::string> resolveLayer(const std::string& layerName, const std::map<std::string, std::string>& keys,
                                                             const std::filesystem::path& presetDir) {
            auto vert = resolveStage(layerName, keys, presetDir, "vertex", "classic/ribbon.vert");
            if (!vert)
                return std::unexpected(vert.error());
            auto frag = resolveStage(layerName, keys, presetDir, "fragment", "classic/ribbon.frag");
            if (!frag)
                return std::unexpected(frag.error());

            SLayerSpec spec;
            spec.name        = layerName;
            spec.vertBuiltin = vert->first;
            spec.vertPath    = vert->second;
            spec.fragBuiltin = frag->first;
            spec.fragPath    = frag->second;
            for (const auto& [k, v] : keys)
                if (k != "vertex" && k != "fragment")
                    spec.defaults[k] = v;
            return spec;
        }

        std::expected<SResolved, std::string> loadInner(const std::string& name) {
            const auto            base     = presetsBaseDir();
            const auto            userFile = base.empty() ? std::filesystem::path{} : base / name / "preset.conf";
            std::filesystem::path presetDir; // empty = built-in (embedded, no real directory)
            std::string           text;

            if (!userFile.empty() && std::filesystem::exists(userFile)) {
                auto read = readFile(userFile);
                if (!read)
                    return std::unexpected(read.error());
                text      = std::move(*read);
                presetDir = userFile.parent_path();
            } else {
                const auto builtin = builtinManifest(name);
                if (builtin.empty())
                    return std::unexpected(std::format("unknown preset \"{}\" (built-in: subtle, classic)", name));
                text = std::string{builtin};
            }

            auto manifest = parse(text);
            if (!manifest)
                return std::unexpected(manifest.error());

            SResolved out;
            out.name        = name;
            out.description = manifest->description;
            static const std::map<std::string, std::string> empty;
            for (const auto& layerName : manifest->layers) {
                const auto it   = manifest->layerKeys.find(layerName);
                auto       spec = resolveLayer(layerName, it == manifest->layerKeys.end() ? empty : it->second, presetDir);
                if (!spec)
                    return std::unexpected(spec.error());
                out.layers.push_back(std::move(*spec));
            }
            return out;
        }

        // Absolute last resort if even the embedded "subtle" manifest somehow
        // fails to parse: a single trail layer on pragma defaults. Never
        // expected to actually run -- it exists so a mistake in this
        // codebase's own built-ins degrades instead of throwing/crashing.
        SResolved hardcodedFallback() {
            SLayerSpec trail{.name = "trail", .vertBuiltin = "classic/ribbon.vert", .fragBuiltin = "classic/ribbon.frag"};
            return SResolved{.name = "subtle", .description = "fallback", .layers = {std::move(trail)}};
        }
    }

    SResolved load(const std::string& name) {
        const auto key = "preset:" + name;
        if (auto r = loadInner(name)) {
            diag::resetKey(key);
            return std::move(*r);
        } else
            diag::report(eSeverity::ERR, key, std::format("preset \"{}\": {}\nUsing \"subtle\" instead.", name, r.error()));

        if (name == "subtle")
            return hardcodedFallback();

        if (auto r = loadInner("subtle"))
            return std::move(*r);
        return hardcodedFallback();
    }
}
