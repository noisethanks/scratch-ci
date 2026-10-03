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

#include "Config.hpp"
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
            std::optional<std::string>              contract, description, sourceKind;
            std::optional<std::vector<std::string>> layers;
        } raw;
        std::map<std::string, std::map<std::string, std::string>> layerKeys;

        std::istringstream                                        in{std::string{text}};
        std::string                                               rawLine;
        int                                                       lineNo = 0;
        while (std::getline(in, rawLine)) {
            ++lineNo;
            const auto       where = std::format("preset.conf:{}", lineNo);

            std::string_view line = rawLine;
            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);
            line = trim(line);
            if (line.empty())
                continue;

            const auto eq = line.find('=');
            if (eq == std::string_view::npos)
                return std::unexpected(std::format(R"({}: malformed line, expected "key = value": "{}")", where, line));
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
                } else if (key == "source") {
                    if (raw.sourceKind)
                        return std::unexpected(std::format("{}: duplicate \"source\"", where));
                    if (!source::known(value))
                        return std::unexpected(std::format("{}: unknown source \"{}\" ({})", where, value, source::kindList()));
                    raw.sourceKind = std::string{value};
                } else
                    return std::unexpected(std::format(R"({}: unknown key "{}" (contract, description, layers, source, or "<layer>:<name>"))", where, key));
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
            for (const auto& l : *raw.layers) {
                if (l == source::KEY_PREFIX)
                    return std::unexpected(std::format("\"{}\" is reserved for the source's settings and can't be a layer name", l));
                if (!seen.insert(l).second)
                    return std::unexpected(std::format(R"("layers" lists "{}" twice)", l));
            }
        }

        // "source:<name>" keys are the source's settings, not a layer's.
        std::map<std::string, std::string> sourceKeys;
        if (const auto it = layerKeys.find(std::string{source::KEY_PREFIX}); it != layerKeys.end()) {
            sourceKeys = std::move(it->second);
            layerKeys.erase(it);
        }
        for (const auto& [layer, keys] : layerKeys) {
            if (std::ranges::find(*raw.layers, layer) == raw.layers->end())
                return std::unexpected(std::format(R"("{}:..." keys given, but "{}" isn't in "layers")", layer, layer));
        }

        return SManifest{.description = raw.description.value_or(""),
                         .layers      = std::move(*raw.layers),
                         .layerKeys   = std::move(layerKeys),
                         .sourceKind  = raw.sourceKind.value_or(std::string{source::DEFAULT_KIND}),
                         .sourceKeys  = std::move(sourceKeys)};
    }

    namespace {
        constexpr unsigned char SUBTLE_CONF[] = {
#embed "../presets/subtle.conf"
        };
        constexpr unsigned char CLASSIC_CONF[] = {
#embed "../presets/classic.conf"
        };
        constexpr unsigned char JITTER_CONF[] = {
#embed "../presets/jitter.conf"
        };
        constexpr unsigned char SPRAY_CONF[] = {
#embed "../presets/spray.conf"
        };
        constexpr unsigned char VIVID_CONF[] = {
#embed "../presets/vivid.conf"
        };
        constexpr unsigned char COMET_CONF[] = {
#embed "../presets/comet.conf"
        };
        constexpr unsigned char EMBERS_CONF[] = {
#embed "../presets/embers.conf"
        };
        constexpr unsigned char SPRING_CONF[] = {
#embed "../presets/spring.conf"
        };

        template <size_t N>
        constexpr std::string_view view(const unsigned char (&data)[N]) {
            return {reinterpret_cast<const char*>(data), N};
        }

        std::string_view builtinManifest(std::string_view name) {
            static const std::map<std::string, std::string_view, std::less<>> m{
                {"subtle", view(SUBTLE_CONF)}, {"classic", view(CLASSIC_CONF)}, {"jitter", view(JITTER_CONF)}, {"spray", view(SPRAY_CONF)},
                {"vivid", view(VIVID_CONF)},   {"comet", view(COMET_CONF)},     {"embers", view(EMBERS_CONF)}, {"spring", view(SPRING_CONF)},
            };
            const auto it = m.find(name);
            return it == m.end() ? std::string_view{} : it->second;
        }

        // Two explicit namespaces, used for preset names and for the shader
        // stages inside a manifest alike: "prefab:<name>" is the embedded
        // built-in, never a file; a bare "<name>" is the user's own, never a
        // built-in. (Shader includes follow the same rule: "helpers/<name>"
        // is the embedded helper, a path is a file. See ShaderSource.cpp.)
        constexpr std::string_view              PREFAB_PREFIX = "prefab:";

        std::expected<std::string, std::string> readFile(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::unexpected(std::format("can't open {}", path.string()));
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Resolves one shader stage of one layer: "prefab:<name>" (a
        // shader::builtin() name), or -- user presets only (allowPaths) -- a
        // path, relative ones against the hyprtail config root, same as
        // layerN_vertex/layerN_fragment (cfg::resolveShaderPath). Always
        // returns a safe built-in identity (`safeBuiltin`) alongside a path
        // override, so CShaderSlot's constructor -- which unconditionally
        // looks up the built-in text -- never sees an unresolvable name (a
        // bad prefab reference is instead caught structurally, right here,
        // before any CLayer/CShaderSlot exists).
        std::expected<std::pair<std::string, std::string>, std::string> resolveStage(const std::string& layerName, const std::map<std::string, std::string>& keys, bool allowPaths,
                                                                                     const char* stageKey, const char* safeBuiltin) {
            const auto it = keys.find(stageKey);
            if (it == keys.end())
                return std::unexpected(std::format(R"(layer "{}" needs "{}:{}")", layerName, layerName, stageKey));
            const std::string& value = it->second;

            if (value.starts_with(PREFAB_PREFIX)) {
                const auto name = value.substr(PREFAB_PREFIX.size());
                if (shader::builtin(name).empty())
                    return std::unexpected(std::format(R"(layer "{}": "{}" isn't a built-in shader)", layerName, value));
                return std::pair<std::string, std::string>{name, ""};
            }

            if (!allowPaths)
                return std::unexpected(std::format(R"(layer "{}": "{}": a prefab preset can only use "prefab:<name>" shaders)", layerName, value));
            if (value.empty())
                return std::unexpected(std::format("layer \"{}:{}\" is empty", layerName, stageKey));

            std::filesystem::path p = cfg::resolveShaderPath(value);
            if (!p.is_absolute())
                return std::unexpected(std::format(R"(layer "{}": can't resolve "{}" (no usable HOME or XDG_CONFIG_HOME))", layerName, value));
            std::error_code ec;
            if (const auto canon = std::filesystem::weakly_canonical(p, ec); !ec)
                p = canon;
            return std::pair<std::string, std::string>{safeBuiltin, p.string()};
        }

        std::expected<SLayerSpec, std::string> resolveLayer(const std::string& layerName, const std::map<std::string, std::string>& keys, bool allowPaths) {
            auto vert = resolveStage(layerName, keys, allowPaths, "vertex", "ribbon.vert");
            if (!vert)
                return std::unexpected(vert.error());
            auto frag = resolveStage(layerName, keys, allowPaths, "fragment", "ribbon.frag");
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

        // "prefab:<name>" -> the embedded manifest; "<name>" ->
        // <hyprtail root>/presets/<name>.conf, no fallback to a built-in.
        std::expected<SResolved, std::string> loadInner(const std::string& name) {
            std::string text;
            bool        prefab = false;

            if (name.starts_with(PREFAB_PREFIX)) {
                prefab             = true;
                const auto builtin = builtinManifest(std::string_view{name}.substr(PREFAB_PREFIX.size()));
                if (builtin.empty())
                    return std::unexpected(std::format("unknown prefab preset \"{}\" (built-in: prefab:subtle, prefab:classic, prefab:jitter, prefab:spray, prefab:vivid, "
                                                       "prefab:comet, prefab:embers, prefab:spring)",
                                                       name));
                text = std::string{builtin};
            } else {
                if (name.empty() || name.contains('/'))
                    return std::unexpected(std::format(
                        R"("{}" isn't a bare preset name; use "prefab:<name>" for a built-in, or a file name without '/' for <hyprtail root>/presets/<name>.conf)", name));
                const auto root = cfg::hyprtailRoot();
                if (root.empty())
                    return std::unexpected("can't locate the hyprtail config directory (no usable HOME or XDG_CONFIG_HOME)");
                const auto file = root / "presets" / (name + ".conf");
                if (!std::filesystem::exists(file))
                    return std::unexpected(
                        std::format("no such file: {}{}", file.string(), builtinManifest(name).empty() ? "" : std::format(" (for the built-in, use \"prefab:{}\")", name)));
                auto read = readFile(file);
                if (!read)
                    return std::unexpected(read.error());
                text = std::move(*read);
            }

            auto manifest = parse(text);
            if (!manifest)
                return std::unexpected(manifest.error());

            SResolved out;
            out.name           = name;
            out.description    = manifest->description;
            out.sourceKind     = manifest->sourceKind;
            out.sourceDefaults = manifest->sourceKeys;
            static const std::map<std::string, std::string> empty;
            for (const auto& layerName : manifest->layers) {
                const auto it   = manifest->layerKeys.find(layerName);
                auto       spec = resolveLayer(layerName, it == manifest->layerKeys.end() ? empty : it->second, !prefab);
                if (!spec)
                    return std::unexpected(spec.error());
                out.layers.push_back(std::move(*spec));
            }
            return out;
        }

        constexpr const char* FALLBACK_PRESET = "prefab:subtle";

        // Absolute last resort if even the embedded "subtle" manifest somehow
        // fails to parse: a single trail layer on pragma defaults. Never
        // expected to actually run -- it exists so a mistake in this
        // codebase's own built-ins degrades instead of throwing/crashing.
        SResolved hardcodedFallback() {
            SLayerSpec trail{.name = "trail", .vertBuiltin = "ribbon.vert", .fragBuiltin = "ribbon.frag"};
            return SResolved{.name = FALLBACK_PRESET, .description = "fallback", .layers = {std::move(trail)}};
        }
    }

    SResolved load(const std::string& name) {
        const auto key = "preset:" + name;
        if (auto r = loadInner(name)) {
            diag::resetKey(key);
            return std::move(*r);
        } else
            diag::report(eSeverity::ERR, key, std::format("preset \"{}\": {}\nUsing \"{}\" instead.", name, r.error(), FALLBACK_PRESET));

        if (name == FALLBACK_PRESET)
            return hardcodedFallback();

        if (auto r = loadInner(FALLBACK_PRESET))
            return std::move(*r);
        return hardcodedFallback();
    }
}
