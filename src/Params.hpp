#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shader-declared parameters and padding expressions (SPEC §13.5). No
// Hyprland headers: pure parsing and arithmetic.
//
//   #pragma hyprtail param <type> <name> <default> [<min> <max>]
//   #pragma hyprtail padding <expr>
//
// Types: float, int, bool, vec2, color. Value syntax (pragma defaults, preset
// and config values): numbers for float/int (min/max apply, to both
// components for vec2), true/false (also 1/0, yes/no) for bool, "x,y" for
// vec2, and for color 0xAARRGGBB, rgba(RRGGBBAA) or rgb(RRGGBB).
//
// Padding expressions: non-negative numbers, parameter names, + - * / and
// parentheses. Nothing else.
namespace hyprtail::params {
    enum class eType : uint8_t {
        FLOAT,
        INT,
        BOOL,
        VEC2,
        COLOR,
    };

    struct SValue {
        eType    type = eType::FLOAT;
        double   x = 0.0, y = 0.0; // FLOAT/INT: x; VEC2: x, y; BOOL: x != 0
        uint32_t argb = 0;         // COLOR, sRGB
    };

    struct SDecl {
        std::string           name;
        eType                 type = eType::FLOAT;
        SValue                def;
        std::optional<double> min, max; // FLOAT, INT, VEC2 only
    };

    const char*                           typeName(eType t);
    const char*                           glslType(eType t);

    // Lowercase letter or '_' first, then letters, digits, '_'. Names starting
    // with ht_ or gl_ are reserved for the prelude and GLSL.
    bool                                  validName(std::string_view name);

    std::expected<SValue, std::string>    parseValue(eType type, std::string_view text);
    std::expected<void, std::string>      checkRange(const SDecl& decl, const SValue& v);
    std::string                           format(const SValue& v);

    // Window-rule effect value truthiness (SPEC §7): true/1/yes/on
    // (case-insensitive) suppress; anything else, including false, empty, or
    // unset, does not. Never fails, unlike parseValue(BOOL) -- a garbled or
    // missing rule value simply doesn't suppress, no warning.
    bool                                   ruleTruthy(std::string_view text);

    // Arguments of a param pragma: "<type> <name> <default> [<min> <max>]".
    std::expected<SDecl, std::string>     parseDecl(std::string_view args);

    // Scalar view of a value for padding expressions: FLOAT/INT/BOOL only.
    std::optional<double>                 scalar(const SValue& v);

    // One "<layer>:<name>=<value>" entry of a `params` config string
    // (SPEC §13.5), unparsed: only the layer/name/value are split out here,
    // not validated against a specific program's declared params.
    struct SParamEntry {
        std::string layer, name, value;
    };

    struct SParsedParams {
        std::vector<SParamEntry> entries;
        std::vector<std::string> problems; // malformed tokens, plain text
    };

    // Applies value text by name onto resolved (declaration, value) pairs:
    // each value is parsed as its declaration's type and range-checked. A
    // name nobody declared, or a bad value, is described in `problems` (one
    // line each, "\n  <name>: ...") and the declared value stays. `owner`
    // names what the parameters belong to ("layer", "source") in the message.
    void applyOverrides(std::vector<std::pair<SDecl, SValue>>& all, const std::map<std::string, std::string>& overrides, std::string_view owner, std::string& problems);

    // Splits on whitespace, then each token on the first ':' and first '='.
    // A malformed token (missing ':' or '=', or an empty layer/name/value)
    // is added to `problems` instead of `entries`; it never fails the whole
    // string.
    SParsedParams                         parseParamsString(std::string_view text);

    class CExpr {
      public:
        static std::expected<CExpr, std::string> parse(std::string_view text);

        // lookup returns a parameter's scalar value, nullopt if unknown.
        std::expected<double, std::string> eval(const std::function<std::optional<double>(std::string_view)>& lookup) const;

        // Every parameter name the expression uses.
        const std::vector<std::string>&    names() const;
        const std::string&                 text() const;

      private:
        struct SToken {
            enum eKind : uint8_t {
                NUMBER,
                NAME,
                ADD,
                SUB,
                MUL,
                DIV,
            } kind = NUMBER;
            double      number = 0.0;
            std::string name;
        };

        std::vector<SToken>      m_rpn; // postfix
        std::vector<std::string> m_names;
        std::string              m_text;
    };
}
