// Unit tests for the Hyprland-free parts (SPEC §13.16): parameter pragmas,
// padding expressions, shader preprocessing (contract 2), node ring.
// `make test-unit` builds and runs this, then validates the preprocessed
// built-in shaders with glslangValidator (written to $OUT_DIR).
//
// No compositor, no GL: runs anywhere.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

#include "../../src/Params.hpp"
#include "../../src/ShaderSource.hpp"
#include "../../src/TrailBuffer.hpp"

using namespace hyprtail;

static int s_failed = 0;
static int s_passed = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (cond)                                                                                                                                                                  \
            ++s_passed;                                                                                                                                                            \
        else {                                                                                                                                                                     \
            ++s_failed;                                                                                                                                                            \
            std::cerr << std::format("FAIL {}:{}: {}\n", __FILE__, __LINE__, #cond);                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static void testParams() {
    using namespace params;

    auto f = parseDecl("float width 8 0 512");
    CHECK(f && f->type == eType::FLOAT && f->name == "width" && f->def.x == 8.0 && f->min == 0.0 && f->max == 512.0);
    CHECK(!parseDecl("float width 600 0 512"));  // default out of range
    CHECK(!parseDecl("float ht_width 1"));       // reserved prefix
    CHECK(!parseDecl("float Width 1"));          // uppercase first
    CHECK(!parseDecl("colour c 0xff000000"));    // unknown type
    CHECK(!parseDecl("color c rgba(ff0000ff) 0 1")); // colors take no range
    CHECK(!parseDecl("float a"));                // missing default

    auto c = parseDecl("color tint rgba(11223344)");
    CHECK(c && c->def.argb == 0x44112233u);
    CHECK(parseValue(eType::COLOR, "0x80ff0000")->argb == 0x80ff0000u);
    CHECK(parseValue(eType::COLOR, "rgb(00ff00)")->argb == 0xff00ff00u);
    CHECK(!parseValue(eType::COLOR, "red"));

    CHECK(parseValue(eType::INT, "3") && !parseValue(eType::INT, "3.5"));
    CHECK(parseValue(eType::BOOL, "true")->x == 1.0 && parseValue(eType::BOOL, "no")->x == 0.0 && !parseValue(eType::BOOL, "maybe"));

    // ruleTruthy: true/1/yes/on (case-insensitive) suppress; everything else,
    // including false/empty/garbage, doesn't -- and never fails.
    CHECK(ruleTruthy("true") && ruleTruthy("1") && ruleTruthy("yes") && ruleTruthy("on"));
    CHECK(ruleTruthy("TRUE") && ruleTruthy("Yes") && ruleTruthy("ON") && ruleTruthy("  true  "));
    CHECK(!ruleTruthy("false") && !ruleTruthy("0") && !ruleTruthy("no") && !ruleTruthy("off"));
    CHECK(!ruleTruthy("") && !ruleTruthy("maybe") && !ruleTruthy("truee"));
    auto v = parseValue(eType::VEC2, "1.5,-2");
    CHECK(v && v->x == 1.5 && v->y == -2.0);
    CHECK(!parseValue(eType::VEC2, "1.5"));

    // Formatting round-trips.
    CHECK(parseValue(eType::COLOR, format(*parseValue(eType::COLOR, "rgba(1a66ffff)")))->argb == 0xff1a66ffu);

    // Expressions.
    const auto lookup = [](std::string_view n) -> std::optional<double> {
        if (n == "width")
            return 8.0;
        if (n == "miter_limit")
            return 2.0;
        return std::nullopt;
    };
    auto e = CExpr::parse("width * 0.5 * miter_limit + 1");
    CHECK(e && e->eval(lookup) && *e->eval(lookup) == 9.0);
    CHECK(e && e->names().size() == 2);
    CHECK(*CExpr::parse("(1 + 2) * 3")->eval(lookup) == 9.0);
    CHECK(*CExpr::parse("1 + 2 * 3")->eval(lookup) == 7.0);
    CHECK(*CExpr::parse("8 - 2 - 1")->eval(lookup) == 5.0); // left-associative
    CHECK(*CExpr::parse("8 / 4 / 2")->eval(lookup) == 1.0);
    CHECK(!CExpr::parse("-1"));            // no unary minus
    CHECK(!CExpr::parse("max(1, 2)"));     // no functions
    CHECK(!CExpr::parse("(1 + 2"));
    CHECK(!CExpr::parse("1 +"));
    CHECK(!CExpr::parse(""));
    CHECK(!CExpr::parse("1 2"));
    CHECK(!CExpr::parse("unknown")->eval(lookup)); // unknown name fails at eval
    CHECK(!CExpr::parse("1 / 0")->eval(lookup));
}

static std::expected<shader::SSource, std::string> pp(const std::string& text, shader::eStage stage) {
    return shader::preprocess(text, "test", {}, stage);
}

static void testShaderSource() {
    using shader::eStage;
    using shader::eTopology;

    const std::string vert = "#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\n#pragma hyprtail param float width 8 0 512\n"
                             "#pragma hyprtail padding width + 1\nvoid main() {}\n";
    auto              v    = pp(vert, eStage::VERTEX);
    CHECK(v.has_value());
    if (v) {
        CHECK(v->topology == eTopology::PATH);
        CHECK(v->params.size() == 1 && v->params[0].decl.name == "width");
        CHECK(v->padding.size() == 1);
        CHECK(v->text.contains("uniform float width;"));
        CHECK(v->text.contains("ht_a_p0Pos"));           // path prelude injected
        CHECK(v->sourceNames.size() == 2);                // main + prelude
    }

    // Missing contract, contract not first, wrong version.
    CHECK(!pp("#version 300 es\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail param float a 1\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 1\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::VERTEX));

    // Topology: required in vertex, forbidden in fragment, one only, known.
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\n#pragma hyprtail topology quad\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology ribbon\nvoid main() {}\n", eStage::VERTEX));

    // Quad prelude.
    auto q = pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology quad\nvoid main() {}\n", eStage::VERTEX);
    CHECK(q && q->topology == eTopology::QUAD && q->text.contains("ht_corner") && !q->text.contains("ht_a_p0Pos"));

    // Reserved and duplicate params, bad padding, unknown pragma.
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail param float fade_ms 1\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail param float a 1\n#pragma hyprtail param float a 1\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail padding max(1, 2)\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail glow 1\nvoid main() {}\n", eStage::FRAGMENT));

    // Built-ins preprocess.
    for (const auto* name : {"classic/ribbon.vert", "classic/ring.vert"})
        CHECK(shader::preprocess(shader::builtin(name), name, {}, eStage::VERTEX).has_value());
    for (const auto* name : {"classic/ribbon.frag", "classic/ring.frag"})
        CHECK(shader::preprocess(shader::builtin(name), name, {}, eStage::FRAGMENT).has_value());
}

static void testRing() {
    CTrailRing ring(8, 42);
    ring.insert({0, 0}, 0.0, false);  // first node: segment start
    ring.insert({3, 4}, 10.0, false); // +5
    ring.insert({6, 8}, 20.0, false); // +5
    ring.insert({100, 100}, 30.0, true); // break: distance restarts
    ring.insert({100, 110}, 40.0, false);

    std::vector<SGpuNode> out;
    ring.orderedCopy(out, 40.0);
    CHECK(out.size() == 5);
    CHECK(out[0].distPx == 0.F && (out[0].bits & GPU_BIT_SEGMENT_START));
    CHECK(out[2].distPx == 10.F && !(out[2].bits & GPU_BIT_SEGMENT_START));
    CHECK(out[3].distPx == 0.F && (out[3].bits & GPU_BIT_SEGMENT_START));
    CHECK(out[4].distPx == 10.F);
    CHECK(out[4].birthMs == 0.F && out[0].birthMs == -40.F);
    // Seeds differ between nodes, and between rings with other bases.
    CHECK((out[1].bits >> 1) != (out[2].bits >> 1));
    CTrailRing other(8, 43);
    other.insert({0, 0}, 0.0, false);
    std::vector<SGpuNode> out2;
    other.orderedCopy(out2, 0.0);
    CHECK((out2[0].bits >> 1) != (out[0].bits >> 1));
}

int main() {
    testParams();
    testShaderSource();
    testRing();

    // Preprocessed built-ins for the GLSL validator.
    if (const char* dir = std::getenv("OUT_DIR")) {
        std::filesystem::create_directories(dir);
        const std::pair<const char*, shader::eStage> builtins[] = {
            {"classic/ribbon.vert", shader::eStage::VERTEX},
            {"classic/ribbon.frag", shader::eStage::FRAGMENT},
            {"classic/ring.vert", shader::eStage::VERTEX},
            {"classic/ring.frag", shader::eStage::FRAGMENT},
        };
        for (const auto& [name, stage] : builtins) {
            auto src = shader::preprocess(shader::builtin(name), name, {}, stage);
            if (!src)
                continue;
            std::string file = name;
            std::ranges::replace(file, '/', '_');
            std::ofstream(std::filesystem::path{dir} / file) << src->text;
        }
    }

    std::cout << std::format("unit: {} passed, {} failed\n", s_passed, s_failed);
    return s_failed ? 1 : 0;
}
