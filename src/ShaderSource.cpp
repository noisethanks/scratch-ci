#include "ShaderSource.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace hyprtail::shader {
    namespace {
        // Stock shaders and the prefab library, embedded at build time (the
        // Makefile lists them as dependencies). Keep them ASCII: GLSL ES
        // drivers aren't reliable with UTF-8, even in comments.
        constexpr unsigned char CLASSIC_RIBBON_VERT[] = {
#embed "../shaders/ribbon.vert"
        };
        constexpr unsigned char CLASSIC_RIBBON_FRAG[] = {
#embed "../shaders/ribbon.frag"
        };
        constexpr unsigned char CLASSIC_RING_VERT[] = {
#embed "../shaders/ring.vert"
        };
        constexpr unsigned char CLASSIC_RING_FRAG[] = {
#embed "../shaders/ring.frag"
        };
        constexpr unsigned char PREFAB_JITTER_VERT[] = {
#embed "../shaders/jitter.vert"
        };
        constexpr unsigned char PREFAB_SPRAY_VERT[] = {
#embed "../shaders/spray.vert"
        };
        constexpr unsigned char PREFAB_DOTS_FRAG[] = {
#embed "../shaders/dots.frag"
        };
        constexpr unsigned char PRELUDE_COMMON[] = {
#embed "../shaders/prelude/common.glsl"
        };
        constexpr unsigned char PRELUDE_VERTEX[] = {
#embed "../shaders/prelude/vertex.glsl"
        };
        constexpr unsigned char PRELUDE_PATH[] = {
#embed "../shaders/prelude/path.glsl"
        };
        constexpr unsigned char PRELUDE_QUAD[] = {
#embed "../shaders/prelude/quad.glsl"
        };
        constexpr unsigned char PRELUDE_INSTANCED[] = {
#embed "../shaders/prelude/instanced.glsl"
        };
        constexpr unsigned char PRELUDE_FRAGMENT[] = {
#embed "../shaders/prelude/fragment.glsl"
        };
        constexpr unsigned char PREFAB_RIBBON[] = {
#embed "../shaders/helpers/ribbon.glsl"
        };
        constexpr unsigned char PREFAB_FADE[] = {
#embed "../shaders/helpers/fade.glsl"
        };
        constexpr unsigned char PREFAB_SDF[] = {
#embed "../shaders/helpers/sdf.glsl"
        };
        constexpr unsigned char PREFAB_NOISE[] = {
#embed "../shaders/helpers/noise.glsl"
        };

        template <size_t N>
        constexpr std::string_view view(const unsigned char (&data)[N]) {
            return {reinterpret_cast<const char*>(data), N};
        }

        const std::map<std::string, std::string_view, std::less<>>& prefabs() {
            static const std::map<std::string, std::string_view, std::less<>> m{
                {"helpers/ribbon.glsl", view(PREFAB_RIBBON)},
                {"helpers/fade.glsl", view(PREFAB_FADE)},
                {"helpers/sdf.glsl", view(PREFAB_SDF)},
                {"helpers/noise.glsl", view(PREFAB_NOISE)},
            };
            return m;
        }

        constexpr int    MAX_DEPTH      = 16;
        constexpr size_t MAX_FILE_BYTES = 1024 * 1024;

        const std::regex RE_INCLUDE{R"re(^\s*#\s*include\s+"([^"]+)"\s*$)re"};
        const std::regex RE_INCLUDE_ANY{R"re(^\s*#\s*include\b)re"};
        const std::regex RE_CONTRACT{R"re(^\s*#\s*pragma\s+hyprtail\s+contract\s+(\S+)\s*$)re"};
        const std::regex RE_TOPOLOGY{R"re(^\s*#\s*pragma\s+hyprtail\s+topology\s+(\S+)(?:\s+(\S+))?\s*$)re"};
        const std::regex RE_EXPECTS{R"re(^\s*#\s*pragma\s+hyprtail\s+expects\s+(\S+)\s*$)re"};
        const std::regex RE_PARAM{R"re(^\s*#\s*pragma\s+hyprtail\s+param\s+(.*)$)re"};
        const std::regex RE_PADDING{R"re(^\s*#\s*pragma\s+hyprtail\s+padding\s+(.*)$)re"};
        const std::regex RE_PRAGMA_HT{R"re(^\s*#\s*pragma\s+hyprtail\b)re"};
        const std::regex RE_VERSION{R"re(^\s*#\s*version\b)re"};

        std::filesystem::path expandHome(const std::string& p) {
            if (p == "~" || p.starts_with("~/")) {
                const char* home = std::getenv("HOME");
                if (home && home[0] == '/')
                    return std::filesystem::path{home} / p.substr(p.size() > 1 ? 2 : 1);
            }
            return p;
        }

        std::expected<std::string, std::string> readFile(const std::filesystem::path& path) {
            std::error_code ec;
            const auto      size = std::filesystem::file_size(path, ec);
            if (ec)
                return std::unexpected(std::format("can't read {}: {}", path.string(), ec.message()));
            if (size > MAX_FILE_BYTES)
                return std::unexpected(std::format("{} is larger than {} bytes", path.string(), MAX_FILE_BYTES));

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::unexpected(std::format("can't open {}", path.string()));

            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        struct SState {
            SSource               out;
            std::set<std::string> done;       // include-once keys
            std::set<std::string> inProgress; // cycle detection
            eStage                stage         = eStage::VERTEX;
            bool                  contractSeen  = false;
            bool                  topologySeen  = false;
        };

        std::string preludeText(eStage stage, std::optional<eTopology> topology) {
            std::string t{view(PRELUDE_COMMON)};
            if (stage == eStage::FRAGMENT)
                return t + std::string{view(PRELUDE_FRAGMENT)};
            t += view(PRELUDE_VERTEX);
            if (topology == eTopology::QUAD)
                t += view(PRELUDE_QUAD);
            else if (topology == eTopology::INSTANCED)
                t += view(PRELUDE_INSTANCED);
            else
                t += view(PRELUDE_PATH);
            return t;
        }

        // A kind name, as used by `expects` (an instanced kind has no K there).
        std::optional<eTopology> parseTopology(std::string_view s) {
            if (s == "path")
                return eTopology::PATH;
            if (s == "quad")
                return eTopology::QUAD;
            if (s == "instanced")
                return eTopology::INSTANCED;
            return std::nullopt;
        }

        struct STopologyDecl {
            eTopology      kind = eTopology::PATH;
            SInstanceCount instances;
        };

        // The arguments of "topology <kind> [<K>]".
        std::expected<STopologyDecl, std::string> parseTopologyDecl(const std::string& kindText, const std::string& arg, bool hasArg) {
            const auto kind = parseTopology(kindText);
            if (!kind)
                return std::unexpected(std::format("unknown topology \"{}\" (path, quad, instanced <K>)", kindText));

            STopologyDecl out{.kind = *kind};
            if (*kind != eTopology::INSTANCED) {
                if (hasArg)
                    return std::unexpected(std::format("topology {} takes no options, got \"{}\"", kindText, arg));
                return out;
            }

            const auto usage = std::format("topology instanced needs K: an integer 1..{} or the name of an int param", MAX_INSTANCES);
            if (!hasArg)
                return std::unexpected(usage);

            if (std::ranges::all_of(arg, [](unsigned char c) { return std::isdigit(c); })) {
                int        k   = 0;
                const auto res = std::from_chars(arg.data(), arg.data() + arg.size(), k);
                if (res.ec != std::errc{} || res.ptr != arg.data() + arg.size() || k < 1 || k > MAX_INSTANCES)
                    return std::unexpected(std::format("instanced K \"{}\" is outside 1..{}", arg, MAX_INSTANCES));
                out.instances.literal = k;
                return out;
            }

            if (!params::validName(arg))
                return std::unexpected(std::format("{}, got \"{}\"", usage, arg));
            out.instances.param = arg;
            return out;
        }

        // "a,b,c" -> ["a", "b", "c"]; no whitespace trimming, matching
        // #pragma hyprtail expects's compact grammar (RE_EXPECTS already
        // requires the whole argument to be one \S+ token).
        std::vector<std::string> splitComma(const std::string& s) {
            std::vector<std::string> out;
            size_t                   start = 0;
            while (true) {
                const auto comma = s.find(',', start);
                out.push_back(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            return out;
        }

        bool isReserved(std::string_view name) {
            return std::ranges::any_of(reservedParams(), [&](const auto& r) { return r.decl.name == name; });
        }

        // One source unit: built-in prefab ("builtin:<name>") or a file path.
        struct SUnit {
            std::string           key;
            std::string           displayName;
            std::string_view      text;
            std::filesystem::path path; // empty for built-ins
        };

        std::expected<void, std::string> process(SState& st, const SUnit& unit, int sourceId, int depth, bool isMain);

        std::expected<SUnit, std::string> resolveInclude(const std::string& target, const SUnit& parent, std::string& storage) {
            if (target.starts_with("helpers/")) {
                const auto it = prefabs().find(target);
                if (it == prefabs().end())
                    return std::unexpected(std::format("unknown built-in prefab \"{}\"", target));
                return SUnit{.key = "builtin:" + target, .displayName = "<" + target + ">", .text = it->second, .path = {}};
            }

            if (parent.path.empty())
                return std::unexpected(std::format("\"{}\": built-in shaders can only include helpers/ prefabs", target));

            std::filesystem::path p = expandHome(target);
            if (p.is_relative())
                p = parent.path.parent_path() / p;

            std::error_code ec;
            const auto      canon = std::filesystem::weakly_canonical(p, ec);
            if (!ec)
                p = canon;

            auto text = readFile(p);
            if (!text)
                return std::unexpected(text.error());

            storage = std::move(*text);
            return SUnit{.key = p.string(), .displayName = p.string(), .text = storage, .path = p};
        }

        std::expected<void, std::string> process(SState& st, const SUnit& unit, int sourceId, int depth, bool isMain) {
            if (depth > MAX_DEPTH)
                return std::unexpected(std::format("{}: include depth limit ({}) exceeded", unit.displayName, MAX_DEPTH));

            st.inProgress.insert(unit.key);
            if (!unit.path.empty())
                st.out.files.push_back(unit.path);

            std::istringstream in{std::string{unit.text}};
            std::string        line;
            int                lineNo = 0;

            while (std::getline(in, line)) {
                ++lineNo;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                const auto where = std::format("{}:{}", unit.displayName, lineNo);
                std::smatch m;

                if (!isMain && std::regex_search(line, RE_VERSION))
                    return std::unexpected(std::format("{}: included files must not contain #version", where));

                if (std::regex_match(line, m, RE_CONTRACT)) {
                    if (!isMain)
                        return std::unexpected(std::format("{}: #pragma hyprtail contract belongs in the main shader file, not an include", where));
                    if (st.contractSeen)
                        return std::unexpected(std::format("{}: duplicate #pragma hyprtail contract", where));
                    if (m[1] != std::to_string(CONTRACT_VERSION))
                        return std::unexpected(std::format("{}: this hyprtail implements contract {}, the shader declares contract {}", where, CONTRACT_VERSION, m[1].str()));
                    st.contractSeen = true;

                    // The prelude, as its own source string, then back here.
                    const int preludeId = static_cast<int>(st.out.sourceNames.size());
                    st.out.sourceNames.push_back("<hyprtail prelude>");
                    st.out.text += std::format("#line 1 {}\n", preludeId);
                    st.out.text += preludeText(st.stage, st.out.topology);
                    st.out.text += std::format("\n#line {} {}\n", lineNo + 1, sourceId);
                    continue;
                }

                const bool htPragma = std::regex_search(line, RE_PRAGMA_HT);
                if ((htPragma || std::regex_search(line, RE_INCLUDE_ANY)) && isMain && !st.contractSeen)
                    return std::unexpected(std::format("{}: \"#pragma hyprtail contract {}\" must come first, right after #version (it brings in the prelude)", where,
                                                       CONTRACT_VERSION));

                if (std::regex_match(line, m, RE_TOPOLOGY)) {
                    // Already read by the pre-scan in preprocess(); only its
                    // placement is checked here.
                    if (!isMain)
                        return std::unexpected(std::format("{}: #pragma hyprtail topology belongs in the main shader file, not an include", where));
                    st.out.text += '\n';
                    continue;
                }

                if (std::regex_match(line, m, RE_EXPECTS)) {
                    // Already read by the pre-scan in preprocess(); only its
                    // placement is checked here.
                    if (!isMain)
                        return std::unexpected(std::format("{}: #pragma hyprtail expects belongs in the main shader file, not an include", where));
                    st.out.text += '\n';
                    continue;
                }

                if (std::regex_match(line, m, RE_PARAM)) {
                    auto decl = params::parseDecl(m[1].str());
                    if (!decl)
                        return std::unexpected(std::format("{}: {}", where, decl.error()));
                    if (isReserved(decl->name))
                        return std::unexpected(std::format("{}: \"{}\" is a reserved parameter; it is always available, don't declare it", where, decl->name));
                    for (const auto& p : st.out.params) {
                        if (p.decl.name == decl->name)
                            return std::unexpected(std::format("{}: param {} already declared at {}", where, decl->name, p.where));
                    }
                    st.out.text += std::format("uniform {} {};\n", params::glslType(decl->type), decl->name);
                    st.out.params.push_back({.decl = std::move(*decl), .where = where});
                    continue;
                }

                if (std::regex_match(line, m, RE_PADDING)) {
                    auto expr = params::CExpr::parse(m[1].str());
                    if (!expr)
                        return std::unexpected(std::format("{}: #pragma hyprtail padding: {}", where, expr.error()));
                    st.out.padding.push_back({.expr = std::move(*expr), .where = where});
                    st.out.text += '\n'; // keep line numbering
                    continue;
                }

                if (htPragma)
                    return std::unexpected(std::format("{}: unknown #pragma hyprtail directive (contract, topology, expects, param, padding)", where));

                if (std::regex_match(line, m, RE_INCLUDE)) {
                    std::string storage;
                    auto        child = resolveInclude(m[1], unit, storage);
                    if (!child)
                        return std::unexpected(std::format("{}: {}", where, child.error()));

                    if (st.inProgress.contains(child->key))
                        return std::unexpected(std::format("{}: include cycle through {}", where, child->displayName));

                    if (st.done.contains(child->key)) {
                        st.out.text += '\n'; // already included once
                        continue;
                    }

                    const int childId = static_cast<int>(st.out.sourceNames.size());
                    st.out.sourceNames.push_back(child->displayName);

                    st.out.text += std::format("#line 1 {}\n", childId);
                    if (auto r = process(st, *child, childId, depth + 1, false); !r)
                        return r;
                    // Next line of this file.
                    st.out.text += std::format("#line {} {}\n", lineNo + 1, sourceId);
                    continue;
                }

                if (std::regex_search(line, RE_INCLUDE_ANY))
                    return std::unexpected(std::format("{}: malformed #include, expected #include \"path\"", where));

                st.out.text += line;
                st.out.text += '\n';
            }

            st.inProgress.erase(unit.key);
            st.done.insert(unit.key);
            return {};
        }
    }

    const char* topologyName(eTopology t) {
        switch (t) {
            case eTopology::QUAD: return "quad";
            case eTopology::INSTANCED: return "instanced";
            case eTopology::PATH: break;
        }
        return "path";
    }

    std::string topologyText(eTopology t, const SInstanceCount& k) {
        if (t != eTopology::INSTANCED)
            return topologyName(t);
        return std::format("instanced {}", k.param.empty() ? std::to_string(k.literal) : k.param);
    }

    std::string_view builtin(std::string_view name) {
        static const std::map<std::string, std::string_view, std::less<>> m{
            {"ribbon.vert", view(CLASSIC_RIBBON_VERT)},
            {"ribbon.frag", view(CLASSIC_RIBBON_FRAG)},
            {"ring.vert", view(CLASSIC_RING_VERT)},
            {"ring.frag", view(CLASSIC_RING_FRAG)},
            {"jitter.vert", view(PREFAB_JITTER_VERT)},
            {"spray.vert", view(PREFAB_SPRAY_VERT)},
            {"dots.frag", view(PREFAB_DOTS_FRAG)},
        };
        const auto it = m.find(name);
        return it == m.end() ? std::string_view{} : it->second;
    }

    std::optional<std::string> expectsMismatch(const SSource& vert, const SSource& frag) {
        const auto topology = vert.topology.value_or(eTopology::PATH);
        if (frag.expects.empty() || std::ranges::find(frag.expects, topology) != frag.expects.end())
            return std::nullopt;

        std::string kinds;
        for (const auto& k : frag.expects)
            kinds += (kinds.empty() ? "" : ",") + std::string{topologyName(k)};
        return std::format("{} expects topology {}; {} declares {}", frag.sourceNames.front(), kinds, vert.sourceNames.front(), topologyText(topology, vert.instances));
    }

    std::optional<std::string> instanceCountProblem(const SSource& vert, const std::vector<params::SDecl>& programParams) {
        if (vert.topology != eTopology::INSTANCED || vert.instances.param.empty())
            return std::nullopt;

        const auto& name = vert.instances.param;
        const auto  it   = std::ranges::find_if(programParams, [&](const auto& d) { return d.name == name; });
        if (it == programParams.end())
            return std::format("{}: instanced K names \"{}\", which isn't a param of this program (declare it with #pragma hyprtail param int {} <default> 1 {})",
                               vert.sourceNames.front(), name, name, MAX_INSTANCES);
        if (it->type != params::eType::INT)
            return std::format("{}: instanced K names \"{}\", which is a {} param; it must be int", vert.sourceNames.front(), name, params::typeName(it->type));
        if (!it->min || !it->max || *it->min < 1.0 || *it->max > MAX_INSTANCES)
            return std::format("{}: instanced K param \"{}\" must declare a range inside 1..{} (#pragma hyprtail param int {} <default> <min> <max>)", vert.sourceNames.front(), name,
                               MAX_INSTANCES, name);
        return std::nullopt;
    }

    const std::vector<std::string>& preludeUniforms() {
        static const std::vector<std::string> u{"ht_proj", "ht_nowMs", "ht_stillMs", "ht_anchor", "ht_extentPx", "ht_K", "fade_ms", "start_ms", "duration_ms"};
        return u;
    }

    const std::vector<int>& preludeAttribLocations(eTopology t) {
        static const std::vector<int> path{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
        static const std::vector<int> instanced{0, 1, 2, 3, 4};
        static const std::vector<int> none{};
        switch (t) {
            case eTopology::PATH: return path;
            case eTopology::INSTANCED: return instanced;
            case eTopology::QUAD: break;
        }
        return none;
    }

    const std::vector<SReserved>& reservedParams() {
        using params::eType;
        const auto f = [](const char* name, double def, double lo, double hi, bool uniform) {
            return SReserved{.decl = {.name = name, .type = eType::FLOAT, .def = {.type = eType::FLOAT, .x = def}, .min = lo, .max = hi}, .uniform = uniform};
        };
        const auto b = [](const char* name, bool def) {
            return SReserved{.decl = {.name = name, .type = eType::BOOL, .def = {.type = eType::BOOL, .x = def ? 1.0 : 0.0}}, .uniform = false};
        };
        static const std::vector<SReserved> r{
            b("enabled", true),
            // Default depends on the topology (path: true, quad: false), see
            // the layer.
            b("draw_when_cursor_hidden", true),
            f("fade_ms", 500.0, 1.0, 60000.0, true),
            f("start_ms", 500.0, 0.0, 60000.0, true),
            f("duration_ms", 1500.0, 0.0, 600000.0, true),
        };
        return r;
    }

    std::expected<SSource, std::string> preprocess(std::string_view mainText, const std::string& name, const std::filesystem::path& path, eStage stage) {
        try {
            SState     st;
            st.stage           = stage;
            const bool builtin = path.empty();
            st.out.sourceNames.push_back(builtin ? "<" + name + ">" : path.string());

            // A copy: sourceNames grows below (prelude, includes).
            const std::string displayName = st.out.sourceNames.front();

            // Topology first: the vertex prelude depends on it, and it's
            // injected at the contract pragma, which comes first.
            {
                std::istringstream in{std::string{mainText}};
                std::string        line;
                int                lineNo = 0;
                while (std::getline(in, line)) {
                    ++lineNo;
                    std::smatch m;
                    const auto  where = std::format("{}:{}", displayName, lineNo);

                    if (std::regex_match(line, m, RE_TOPOLOGY)) {
                        if (stage == eStage::FRAGMENT)
                            return std::unexpected(std::format("{}: #pragma hyprtail topology belongs in the geometry (vertex) shader", where));
                        if (st.out.topology)
                            return std::unexpected(std::format("{}: duplicate #pragma hyprtail topology", where));
                        const auto decl = parseTopologyDecl(m[1].str(), m[2].str(), m[2].matched);
                        if (!decl)
                            return std::unexpected(std::format("{}: {}", where, decl.error()));
                        st.out.topology  = decl->kind;
                        st.out.instances = decl->instances;
                        continue;
                    }

                    if (std::regex_match(line, m, RE_EXPECTS)) {
                        if (stage == eStage::VERTEX)
                            return std::unexpected(std::format("{}: #pragma hyprtail expects belongs in a fragment (shading) shader", where));
                        if (!st.out.expectsWhere.empty())
                            return std::unexpected(std::format("{}: duplicate #pragma hyprtail expects", where));
                        st.out.expectsWhere = where;
                        for (const auto& kind : splitComma(m[1].str())) {
                            const auto t = parseTopology(kind);
                            if (!t)
                                return std::unexpected(std::format("{}: unknown topology \"{}\" in #pragma hyprtail expects (path, quad, instanced)", where, kind));
                            st.out.expects.push_back(*t);
                        }
                    }
                }
            }

            const SUnit unit{.key = builtin ? "builtin-main:" + name : path.string(), .displayName = displayName, .text = mainText, .path = path};
            if (auto r = process(st, unit, 0, 0, true); !r)
                return std::unexpected(r.error());

            if (!st.contractSeen)
                return std::unexpected(std::format("{}: missing \"#pragma hyprtail contract {}\" after #version. Shaders written for earlier hyprtail versions need "
                                                   "porting; the classic preset's shaders are the reference",
                                                   displayName, CONTRACT_VERSION));
            if (stage == eStage::VERTEX && !st.out.topology)
                return std::unexpected(std::format("{}: geometry (vertex) shaders need \"#pragma hyprtail topology path\", \"quad\" or \"instanced <K>\"", displayName));

            return std::move(st.out);
        } catch (const std::exception& e) { return std::unexpected(std::format("{}: preprocessing failed: {}", name, e.what())); }
    }

    std::expected<SSource, std::string> load(const std::filesystem::path& path, eStage stage) {
        auto text = readFile(path);
        if (!text)
            return std::unexpected(text.error());
        return preprocess(*text, path.string(), path, stage);
    }

    std::string mapLog(const std::string& log, const SSource& src) {
        // Mesa "0:12(5): error", ANGLE-style "ERROR: 0:12:", NVIDIA "0(12) :".
        static const std::regex RE_REF{R"re((^|[\s:])(\d+)(?::(\d+)|\((\d+)\)))re", std::regex::ECMAScript | std::regex::multiline};

        std::string out;
        auto        begin = log.cbegin();
        std::smatch m;
        // After the first match, the text before `begin` is real context:
        // don't let ^ match mid-line there.
        auto flags = std::regex_constants::match_default;
        while (std::regex_search(begin, log.cend(), m, RE_REF, flags)) {
            flags = std::regex_constants::match_prev_avail;
            out.append(begin, m[0].first);
            const size_t id   = std::stoul(m[2]);
            const auto   line = m[3].matched ? m[3].str() : m[4].str();
            if (id < src.sourceNames.size())
                out += std::format("{}{}:{}", m[1].str(), src.sourceNames[id], line);
            else
                out += m[0].str();
            begin = m[0].second;
        }
        out.append(begin, log.cend());
        return out;
    }
}
