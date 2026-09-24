#include "ShaderSource.hpp"

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
        constexpr unsigned char TRAIL_VERT[] = {
#embed "../shaders/trail.vert"
        };
        constexpr unsigned char TRAIL_FRAG[] = {
#embed "../shaders/trail.frag"
        };
        constexpr unsigned char PREFAB_RIBBON[] = {
#embed "../shaders/hyprtail/ribbon.glsl"
        };
        constexpr unsigned char PREFAB_FADE[] = {
#embed "../shaders/hyprtail/fade.glsl"
        };
        constexpr unsigned char PREFAB_SDF[] = {
#embed "../shaders/hyprtail/sdf.glsl"
        };
        constexpr unsigned char IDLE_VERT[] = {
#embed "../shaders/idle.vert"
        };
        constexpr unsigned char IDLE_FRAG[] = {
#embed "../shaders/idle.frag"
        };

        template <size_t N>
        constexpr std::string_view view(const unsigned char (&data)[N]) {
            return {reinterpret_cast<const char*>(data), N};
        }

        const std::map<std::string, std::string_view, std::less<>>& prefabs() {
            static const std::map<std::string, std::string_view, std::less<>> m{
                {"hyprtail/ribbon.glsl", view(PREFAB_RIBBON)},
                {"hyprtail/fade.glsl", view(PREFAB_FADE)},
                {"hyprtail/sdf.glsl", view(PREFAB_SDF)},
            };
            return m;
        }

        constexpr int    MAX_DEPTH      = 16;
        constexpr size_t MAX_FILE_BYTES = 1024 * 1024;

        const std::regex RE_INCLUDE{R"re(^\s*#\s*include\s+"([^"]+)"\s*$)re"};
        const std::regex RE_INCLUDE_ANY{R"re(^\s*#\s*include\b)re"};
        const std::regex RE_PADDING{R"re(^\s*#\s*pragma\s+hyprtail\s+padding\s+(\S+)\s*$)re"};
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
        };

        // One source unit: built-in prefab ("builtin:<name>") or a file path.
        struct SUnit {
            std::string           key;
            std::string           displayName;
            std::string_view      text;
            std::filesystem::path path; // empty for built-ins
        };

        std::expected<void, std::string> process(SState& st, const SUnit& unit, int sourceId, int depth, bool isMain);

        std::expected<SUnit, std::string> resolveInclude(const std::string& target, const SUnit& parent, std::string& storage) {
            if (target.starts_with("hyprtail/")) {
                const auto it = prefabs().find(target);
                if (it == prefabs().end())
                    return std::unexpected(std::format("unknown built-in prefab \"{}\"", target));
                return SUnit{.key = "builtin:" + target, .displayName = "<" + target + ">", .text = it->second, .path = {}};
            }

            if (parent.path.empty())
                return std::unexpected(std::format("\"{}\": built-in shaders can only include hyprtail/ prefabs", target));

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

                if (std::regex_match(line, m, RE_PADDING)) {
                    const std::string val = m[1];
                    float             px  = 0.F;
                    const auto [ptr, ec]  = std::from_chars(val.data(), val.data() + val.size(), px);
                    if (ec != std::errc{} || ptr != val.data() + val.size() || px < 0.F || px > 4096.F)
                        return std::unexpected(std::format("{}: #pragma hyprtail padding needs a number of pixels in 0..4096, got \"{}\"", where, val));
                    st.out.declaredPaddingPx = std::max(st.out.declaredPaddingPx, px);
                    st.out.text += '\n'; // keep line numbering
                    continue;
                }

                if (std::regex_search(line, RE_PRAGMA_HT))
                    return std::unexpected(std::format("{}: unknown #pragma hyprtail directive", where));

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

    std::string_view builtinVertex() {
        return view(TRAIL_VERT);
    }

    std::string_view builtinFragment() {
        return view(TRAIL_FRAG);
    }

    std::string_view builtinIdleVertex() {
        return view(IDLE_VERT);
    }

    std::string_view builtinIdleFragment() {
        return view(IDLE_FRAG);
    }

    std::expected<SSource, std::string> preprocess(std::string_view mainText, const std::string& name, const std::filesystem::path& path) {
        try {
            SState     st;
            const bool builtin = path.empty();
            st.out.sourceNames.push_back(builtin ? "<" + name + ">" : path.string());

            const SUnit unit{.key = builtin ? "builtin-main:" + name : path.string(), .displayName = st.out.sourceNames.front(), .text = mainText, .path = path};
            if (auto r = process(st, unit, 0, 0, true); !r)
                return std::unexpected(r.error());

            return std::move(st.out);
        } catch (const std::exception& e) { return std::unexpected(std::format("{}: preprocessing failed: {}", name, e.what())); }
    }

    std::expected<SSource, std::string> load(const std::filesystem::path& path) {
        auto text = readFile(path);
        if (!text)
            return std::unexpected(text.error());
        return preprocess(*text, path.string(), path);
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
