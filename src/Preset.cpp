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
        constexpr unsigned char JITTER_CONF[] = {
#embed "../hyprtail/presets/jitter.conf"
        };
        constexpr unsigned char VIVID_CONF[] = {
#embed "../hyprtail/presets/vivid.conf"
        };
        constexpr unsigned char COMET_CONF[] = {
#embed "../hyprtail/presets/comet.conf"
        };
        constexpr unsigned char EMBERS_CONF[] = {
#embed "../hyprtail/presets/embers.conf"
        };
        constexpr unsigned char SPRING_CONF[] = {
#embed "../hyprtail/presets/spring.conf"
        };
        constexpr unsigned char INK_CONF[] = {
#embed "../hyprtail/presets/ink.conf"
        };

        template <size_t N>
        constexpr std::string_view view(const unsigned char (&data)[N]) {
            return {reinterpret_cast<const char*>(data), N};
        }

        std::string_view builtinManifest(std::string_view name) {
            static const std::map<std::string, std::string_view, std::less<>> m{
                {"jitter", view(JITTER_CONF)}, {"vivid", view(VIVID_CONF)},
                {"comet", view(COMET_CONF)},   {"embers", view(EMBERS_CONF)},   {"spring", view(SPRING_CONF)}, {"ink", view(INK_CONF)},
            };
            const auto it = m.find(name);
            return it == m.end() ? std::string_view{} : it->second;
        }

        // "prefab:<name>" is the embedded built-in, never a file. As a trail
        // it names an embedded preset; as a shader stage (user presets only)
        // it is shorthand for the embedded "shaders/<name>". Anything else
        // is a path, resolved by cfg::resolveShaderPath(): relative ones
        // against the hyprtail root, "~" and absolute as given. (Shader
        // includes follow the same rule: "helpers/<name>" is the embedded
        // helper, a path is a file. See ShaderSource.cpp.)
        constexpr std::string_view              PREFAB_PREFIX = "prefab:";

        std::expected<std::string, std::string> readFile(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::unexpected(std::format("can't open {}", path.string()));
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Resolves one shader stage of one layer to {embedded key, path
        // override}; a path override is "" for an embedded shader.
        //  - "prefab:<name>": the embedded shaders/<name>, in any preset.
        //  - anything else in an embedded preset: looked up in the embedded
        //    shader table by the path as written ("shaders/ribbon.vert"),
        //    never on disk. That keeps the zero-file first run working and a
        //    stale copied folder from changing what prefab:<preset> means.
        //  - anything else in a file preset: a path, resolved like
        //    layerN_vertex/layerN_fragment (cfg::resolveShaderPath). One that
        //    isn't on disk is added to `missing`; load() reports it.
        // Always returns a safe embedded identity (`safeBuiltin`) alongside a
        // path override, so CShaderSlot's constructor -- which
        // unconditionally looks up the embedded text -- never sees an
        // unresolvable name (a bad embedded reference is instead caught
        // structurally, right here, before any CLayer/CShaderSlot exists).
        std::expected<std::pair<std::string, std::string>, std::string> resolveStage(const std::string& layerName, const std::map<std::string, std::string>& keys, bool embedded,
                                                                                     const char* stageKey, const char* safeBuiltin, std::vector<std::string>& missing) {
            const auto it = keys.find(stageKey);
            if (it == keys.end())
                return std::unexpected(std::format(R"(layer "{}" needs "{}:{}")", layerName, layerName, stageKey));
            const std::string& value = it->second;

            if (value.starts_with(PREFAB_PREFIX)) {
                auto key = "shaders/" + value.substr(PREFAB_PREFIX.size());
                if (shader::builtin(key).empty())
                    return std::unexpected(std::format(R"(layer "{}": "{}" isn't a built-in shader)", layerName, value));
                return std::pair<std::string, std::string>{std::move(key), ""};
            }

            if (value.empty())
                return std::unexpected(std::format("layer \"{}:{}\" is empty", layerName, stageKey));

            if (embedded) {
                if (shader::builtin(value).empty())
                    return std::unexpected(std::format(R"(layer "{}": "{}" isn't an embedded shader; a prefab preset names them like "shaders/ribbon.vert")", layerName, value));
                return std::pair<std::string, std::string>{value, ""};
            }

            std::filesystem::path p = cfg::resolveShaderPath(value);
            if (!p.is_absolute())
                return std::unexpected(std::format(R"(layer "{}": can't resolve "{}" (no usable HOME or XDG_CONFIG_HOME))", layerName, value));
            std::error_code ec;
            if (const auto canon = std::filesystem::weakly_canonical(p, ec); !ec)
                p = canon;
            if (!std::filesystem::exists(p))
                missing.push_back(p.string());
            return std::pair<std::string, std::string>{safeBuiltin, p.string()};
        }

        std::expected<SLayerSpec, std::string> resolveLayer(const std::string& layerName, const std::map<std::string, std::string>& keys, bool embedded,
                                                            std::vector<std::string>& missing) {
            auto vert = resolveStage(layerName, keys, embedded, "vertex", "shaders/ribbon.vert", missing);
            if (!vert)
                return std::unexpected(vert.error());
            auto frag = resolveStage(layerName, keys, embedded, "fragment", "shaders/gradient.frag", missing);
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

        // What to write instead of a trail that is neither "prefab:<name>"
        // nor a .conf path (the old bare "<name>" form, no extension).
        std::string notATrail(const std::string& name) {
            if (name.empty())
                return R"(trail is empty; write "prefab:<name>" for a built-in, or the path of a .conf file relative to <hyprtail root>, e.g. "presets/mine.conf")";
            const auto own  = name.contains('/') ? name + ".conf" : "presets/" + name + ".conf";
            const auto hint = builtinManifest(name).empty() ? std::string{} : std::format(R"("prefab:{}" for the built-in, or )", name);
            return std::format(R"("{}" isn't a trail: write {}"{}" for your own file (the path of a .conf file with its extension, relative to <hyprtail root>))", name, hint, own);
        }

        // "prefab:<name>" -> the embedded manifest; otherwise the path of a
        // .conf file (cfg::resolveShaderPath, relative ones against the
        // hyprtail root), never a fallback to a built-in. Shader files a file
        // preset names that aren't on disk are added to `missing`.
        std::expected<SResolved, std::string> loadInner(const std::string& name, std::vector<std::string>& missing) {
            std::string text;
            bool        embedded = false;

            if (name.starts_with(PREFAB_PREFIX)) {
                embedded           = true;
                const auto builtin = builtinManifest(std::string_view{name}.substr(PREFAB_PREFIX.size()));
                if (builtin.empty())
                    return std::unexpected(std::format("unknown prefab preset \"{}\" (built-in: prefab:jitter, prefab:vivid, "
                                                       "prefab:comet, prefab:embers, prefab:spring, prefab:ink)",
                                                       name));
                text = std::string{builtin};
            } else {
                if (!name.ends_with(".conf"))
                    return std::unexpected(notATrail(name));
                const auto file = cfg::resolveShaderPath(name);
                if (!file.is_absolute())
                    return std::unexpected("can't locate the hyprtail config directory (no usable HOME or XDG_CONFIG_HOME)");
                if (!std::filesystem::exists(file)) {
                    const auto stem = std::filesystem::path{name}.stem().string();
                    return std::unexpected(
                        std::format("no such file: {}{}", file.string(), builtinManifest(stem).empty() ? "" : std::format(" (for the built-in, use \"prefab:{}\")", stem)));
                }
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
                auto       spec = resolveLayer(layerName, it == manifest->layerKeys.end() ? empty : it->second, embedded, missing);
                if (!spec)
                    return std::unexpected(spec.error());
                out.layers.push_back(std::move(*spec));
            }
            return out;
        }

        constexpr const char* FALLBACK_PRESET = "prefab:ink";

        // Absolute last resort if even the embedded "ink" manifest somehow
        // fails to parse: a single trail layer on pragma defaults. Never
        // expected to actually run -- it exists so a mistake in this
        // codebase's own built-ins degrades instead of throwing/crashing.
        SResolved hardcodedFallback() {
            SLayerSpec trail{.name = "trail", .vertBuiltin = "shaders/ribbon.vert", .fragBuiltin = "shaders/gradient.frag"};
            return SResolved{.name = FALLBACK_PRESET, .description = "fallback", .layers = {std::move(trail)}};
        }
    }

    std::optional<SResolved> load(const std::string& name, bool haveActive) {
        const auto               key = "trail:" + name;
        std::vector<std::string> missing;
        auto                     r = loadInner(name, missing);

        if (r && missing.empty()) {
            diag::resetKey(key);
            return std::move(*r);
        }

        if (!r)
            diag::report(eSeverity::ERR, key, std::format("trail \"{}\": {}\nUsing \"{}\" instead.", name, r.error(), FALLBACK_PRESET));
        else {
            // The preset itself is fine, but shader files it names aren't on
            // disk: degraded, not off, so a warning, and what is showing
            // stays (nothing is showing yet at startup: the fallback).
            std::string list;
            for (const auto& m : missing)
                list += "\n  " + m;
            diag::report(eSeverity::WARN, key,
                         std::format("trail \"{}\": shader file(s) not found:{}\n{}", name, list, haveActive ? "Keeping the current trail." : std::format("Using \"{}\" instead.", FALLBACK_PRESET)));
            if (haveActive)
                return std::nullopt;
        }

        if (name == FALLBACK_PRESET)
            return hardcodedFallback();

        std::vector<std::string> none;
        if (auto fallback = loadInner(FALLBACK_PRESET, none))
            return std::move(*fallback);
        return hardcodedFallback();
    }
}
