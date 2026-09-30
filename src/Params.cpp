#include "Params.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace hyprtail::params {
    namespace {
        std::string_view trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        std::vector<std::string_view> splitWs(std::string_view s) {
            std::vector<std::string_view> out;
            size_t                        i = 0;
            while (i < s.size()) {
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                    ++i;
                const size_t start = i;
                while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
                    ++i;
                if (i > start)
                    out.push_back(s.substr(start, i - start));
            }
            return out;
        }

        std::optional<double> parseNumber(std::string_view s) {
            s        = trim(s);
            double v = 0.0;
            if (s.empty())
                return std::nullopt;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            if (ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(v))
                return std::nullopt;
            return v;
        }

        std::optional<uint32_t> parseHex(std::string_view s, size_t digits) {
            if (s.size() != digits)
                return std::nullopt;
            uint32_t v           = 0;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
            if (ec != std::errc{} || ptr != s.data() + s.size())
                return std::nullopt;
            return v;
        }

        std::optional<uint32_t> parseColor(std::string_view s) {
            s = trim(s);
            if (s.starts_with("0x") || s.starts_with("0X"))
                return parseHex(s.substr(2), 8);
            if (s.starts_with("rgba(") && s.ends_with(")")) {
                const auto rgba = parseHex(s.substr(5, s.size() - 6), 8);
                if (!rgba)
                    return std::nullopt;
                return (*rgba >> 8) | (*rgba << 24); // RRGGBBAA -> AARRGGBB
            }
            if (s.starts_with("rgb(") && s.ends_with(")")) {
                const auto rgb = parseHex(s.substr(4, s.size() - 5), 6);
                if (!rgb)
                    return std::nullopt;
                return 0xFF000000u | *rgb;
            }
            return std::nullopt;
        }
    }

    const char* typeName(eType t) {
        switch (t) {
            case eType::FLOAT: return "float";
            case eType::INT: return "int";
            case eType::BOOL: return "bool";
            case eType::VEC2: return "vec2";
            case eType::COLOR: return "color";
        }
        return "?";
    }

    const char* glslType(eType t) {
        switch (t) {
            case eType::FLOAT: return "float";
            case eType::INT: return "int";
            case eType::BOOL: return "bool";
            case eType::VEC2: return "vec2";
            case eType::COLOR: return "vec4";
        }
        return "float";
    }

    bool validName(std::string_view name) {
        if (name.empty() || name.size() > 64)
            return false;
        if (!(std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_'))
            return false;
        if (!std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
            return false;
        return !name.starts_with("ht_") && !name.starts_with("gl_") && name.find("__") == std::string_view::npos;
    }

    std::expected<SValue, std::string> parseValue(eType type, std::string_view text) {
        text = trim(text);
        SValue v{.type = type};
        switch (type) {
            case eType::FLOAT: {
                const auto n = parseNumber(text);
                if (!n)
                    return std::unexpected(std::format("\"{}\" is not a number", text));
                v.x = *n;
                return v;
            }
            case eType::INT: {
                const auto n = parseNumber(text);
                if (!n || *n != std::floor(*n))
                    return std::unexpected(std::format("\"{}\" is not an integer", text));
                v.x = *n;
                return v;
            }
            case eType::BOOL: {
                if (text == "true" || text == "1" || text == "yes")
                    v.x = 1.0;
                else if (text == "false" || text == "0" || text == "no")
                    v.x = 0.0;
                else
                    return std::unexpected(std::format("\"{}\" is not a bool (true or false)", text));
                return v;
            }
            case eType::VEC2: {
                const auto comma = text.find(',');
                const auto a     = comma == std::string_view::npos ? std::nullopt : parseNumber(text.substr(0, comma));
                const auto b     = comma == std::string_view::npos ? std::nullopt : parseNumber(text.substr(comma + 1));
                if (!a || !b)
                    return std::unexpected(std::format("\"{}\" is not a vec2 (x,y)", text));
                v.x = *a;
                v.y = *b;
                return v;
            }
            case eType::COLOR: {
                const auto c = parseColor(text);
                if (!c)
                    return std::unexpected(std::format("\"{}\" is not a color (0xAARRGGBB, rgba(RRGGBBAA) or rgb(RRGGBB))", text));
                v.argb = *c;
                return v;
            }
        }
        return std::unexpected("unknown type");
    }

    bool ruleTruthy(std::string_view text) {
        text = trim(text);
        std::string lower{text};
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
    }

    std::expected<void, std::string> checkRange(const SDecl& decl, const SValue& v) {
        if (decl.type != eType::FLOAT && decl.type != eType::INT && decl.type != eType::VEC2)
            return {};
        const auto in = [&](double x) { return (!decl.min || x >= *decl.min) && (!decl.max || x <= *decl.max); };
        if (in(v.x) && (decl.type != eType::VEC2 || in(v.y)))
            return {};
        return std::unexpected(std::format("{} is outside {}..{}", format(v), decl.min ? std::format("{}", *decl.min) : "", decl.max ? std::format("{}", *decl.max) : ""));
    }

    std::string format(const SValue& v) {
        switch (v.type) {
            case eType::FLOAT: return std::format("{}", v.x);
            case eType::INT: return std::format("{}", static_cast<int64_t>(v.x));
            case eType::BOOL: return v.x != 0.0 ? "true" : "false";
            case eType::VEC2: return std::format("{},{}", v.x, v.y);
            case eType::COLOR: return std::format("0x{:08x}", v.argb);
        }
        return "?";
    }

    std::expected<SDecl, std::string> parseDecl(std::string_view args) {
        const auto parts = splitWs(args);
        if (parts.size() != 3 && parts.size() != 5)
            return std::unexpected("expected: #pragma hyprtail param <type> <name> <default> [<min> <max>]");

        SDecl d;
        if (parts[0] == "float")
            d.type = eType::FLOAT;
        else if (parts[0] == "int")
            d.type = eType::INT;
        else if (parts[0] == "bool")
            d.type = eType::BOOL;
        else if (parts[0] == "vec2")
            d.type = eType::VEC2;
        else if (parts[0] == "color")
            d.type = eType::COLOR;
        else
            return std::unexpected(std::format("unknown param type \"{}\" (float, int, bool, vec2, color)", parts[0]));

        d.name = parts[1];
        if (!validName(d.name))
            return std::unexpected(std::format("invalid param name \"{}\" (lowercase letters, digits, _; not starting with ht_ or gl_)", d.name));

        auto def = parseValue(d.type, parts[2]);
        if (!def)
            return std::unexpected(std::format("param {}: default {}", d.name, def.error()));
        d.def = *def;

        if (parts.size() == 5) {
            if (d.type == eType::BOOL || d.type == eType::COLOR)
                return std::unexpected(std::format("param {}: {} params take no range", d.name, typeName(d.type)));
            d.min = parseNumber(parts[3]);
            d.max = parseNumber(parts[4]);
            if (!d.min || !d.max || *d.min > *d.max)
                return std::unexpected(std::format("param {}: invalid range {} {}", d.name, parts[3], parts[4]));
            if (auto r = checkRange(d, d.def); !r)
                return std::unexpected(std::format("param {}: default {}", d.name, r.error()));
        }
        return d;
    }

    std::optional<double> scalar(const SValue& v) {
        if (v.type == eType::FLOAT || v.type == eType::INT || v.type == eType::BOOL)
            return v.x;
        return std::nullopt;
    }

    void applyOverrides(std::vector<std::pair<SDecl, SValue>>& all, const std::map<std::string, std::string>& overrides, std::string_view owner, std::string& problems) {
        for (const auto& [name, text] : overrides) {
            const auto it = std::ranges::find_if(all, [&](const auto& e) { return e.first.name == name; });
            if (it == all.end()) {
                problems += std::format("\n  {}: not a parameter of this {}; ignoring", name, owner);
                continue;
            }
            auto v = parseValue(it->first.type, text);
            if (v)
                if (auto r = checkRange(it->first, *v); !r)
                    v = std::unexpected(r.error());
            if (!v) {
                problems += std::format("\n  {}: {}; using {}", name, v.error(), format(it->second));
                continue;
            }
            it->second = *v;
        }
    }

    SParsedParams parseParamsString(std::string_view text) {
        SParsedParams out;
        for (const auto tok : splitWs(text)) {
            const auto colon = tok.find(':');
            const auto eq    = tok.find('=');
            if (colon == std::string_view::npos || eq == std::string_view::npos || eq < colon) {
                out.problems.push_back(std::format("\"{}\" is not <layer>:<name>=<value>", tok));
                continue;
            }
            const auto layer = trim(tok.substr(0, colon));
            const auto name  = trim(tok.substr(colon + 1, eq - colon - 1));
            const auto value = tok.substr(eq + 1);
            if (layer.empty() || name.empty()) {
                out.problems.push_back(std::format("\"{}\" is not <layer>:<name>=<value>", tok));
                continue;
            }
            out.entries.push_back({.layer = std::string{layer}, .name = std::string{name}, .value = std::string{value}});
        }
        return out;
    }

    // ---------------------------------------------------------------- expressions

    std::expected<CExpr, std::string> CExpr::parse(std::string_view text) {
        CExpr e;
        e.m_text = std::string{trim(text)};

        // Tokenize.
        struct STok {
            char   op = 0; // 0 = operand, else one of + - * / ( )
            SToken operand;
        };
        std::vector<STok> toks;
        const auto&       s = e.m_text;
        for (size_t i = 0; i < s.size();) {
            const char c = s[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')') {
                toks.push_back({.op = c});
                ++i;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
                size_t j = i;
                while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.'))
                    ++j;
                const auto n = parseNumber(std::string_view{s}.substr(i, j - i));
                if (!n)
                    return std::unexpected(std::format("bad number \"{}\"", s.substr(i, j - i)));
                toks.push_back({.op = 0, .operand = {.kind = SToken::NUMBER, .number = *n}});
                i = j;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i;
                while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                    ++j;
                toks.push_back({.op = 0, .operand = {.kind = SToken::NAME, .name = s.substr(i, j - i)}});
                i = j;
                continue;
            }
            return std::unexpected(std::format("unexpected '{}' (only numbers, parameter names, + - * / and parentheses)", c));
        }
        if (toks.empty())
            return std::unexpected("empty expression");

        // Shunting-yard with an operand/operator alternation check (no unary
        // operators).
        const auto prec = [](char op) { return op == '*' || op == '/' ? 2 : 1; };
        const auto kind = [](char op) {
            switch (op) {
                case '+': return SToken::ADD;
                case '-': return SToken::SUB;
                case '*': return SToken::MUL;
                default: return SToken::DIV;
            }
        };
        std::vector<char> ops;
        bool              expectOperand = true;
        for (const auto& t : toks) {
            if (t.op == 0) {
                if (!expectOperand)
                    return std::unexpected("missing operator between operands");
                if (t.operand.kind == SToken::NAME && std::ranges::find(e.m_names, t.operand.name) == e.m_names.end())
                    e.m_names.push_back(t.operand.name);
                e.m_rpn.push_back(t.operand);
                expectOperand = false;
            } else if (t.op == '(') {
                if (!expectOperand)
                    return std::unexpected("missing operator before '('");
                ops.push_back('(');
            } else if (t.op == ')') {
                if (expectOperand)
                    return std::unexpected("missing operand before ')'");
                while (!ops.empty() && ops.back() != '(') {
                    e.m_rpn.push_back({.kind = kind(ops.back())});
                    ops.pop_back();
                }
                if (ops.empty())
                    return std::unexpected("unbalanced ')'");
                ops.pop_back();
            } else {
                if (expectOperand)
                    return std::unexpected(std::format("missing operand before '{}' (no unary operators)", t.op));
                while (!ops.empty() && ops.back() != '(' && prec(ops.back()) >= prec(t.op)) {
                    e.m_rpn.push_back({.kind = kind(ops.back())});
                    ops.pop_back();
                }
                ops.push_back(t.op);
                expectOperand = true;
            }
        }
        if (expectOperand)
            return std::unexpected("expression ends with an operator");
        while (!ops.empty()) {
            if (ops.back() == '(')
                return std::unexpected("unbalanced '('");
            e.m_rpn.push_back({.kind = kind(ops.back())});
            ops.pop_back();
        }
        return e;
    }

    std::expected<double, std::string> CExpr::eval(const std::function<std::optional<double>(std::string_view)>& lookup) const {
        std::vector<double> stack;
        for (const auto& t : m_rpn) {
            if (t.kind == SToken::NUMBER) {
                stack.push_back(t.number);
                continue;
            }
            if (t.kind == SToken::NAME) {
                const auto v = lookup(t.name);
                if (!v)
                    return std::unexpected(std::format("unknown parameter \"{}\" (or not a float, int or bool)", t.name));
                stack.push_back(*v);
                continue;
            }
            if (stack.size() < 2)
                return std::unexpected("malformed expression");
            const double b = stack.back();
            stack.pop_back();
            const double a = stack.back();
            stack.pop_back();
            switch (t.kind) {
                case SToken::ADD: stack.push_back(a + b); break;
                case SToken::SUB: stack.push_back(a - b); break;
                case SToken::MUL: stack.push_back(a * b); break;
                case SToken::DIV:
                    if (b == 0.0)
                        return std::unexpected("division by zero");
                    stack.push_back(a / b);
                    break;
                default: return std::unexpected("malformed expression");
            }
        }
        if (stack.size() != 1 || !std::isfinite(stack.back()))
            return std::unexpected("malformed expression");
        return stack.back();
    }

    const std::vector<std::string>& CExpr::names() const {
        return m_names;
    }

    const std::string& CExpr::text() const {
        return m_text;
    }
}
