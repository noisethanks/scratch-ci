// Unit tests for the Hyprland-free parts (SPEC §13.16): parameter pragmas,
// padding expressions, shader preprocessing (contract 2), node ring.
// `make test-unit` builds and runs this, then validates the preprocessed
// built-in shaders with glslangValidator (written to $OUT_DIR).
//
// No compositor, no GL: runs anywhere.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../src/ConfigParse.hpp"
#include "../../src/CrashGuard.hpp"
#include "../../src/Params.hpp"
#include "../../src/PointerGate.hpp"
#include "../../src/ShaderSource.hpp"
#include "../../src/Source.hpp"
#include "../../src/SpringChain.hpp"
#include "../../src/TrailBuffer.hpp"

#include <sys/wait.h>
#include <unistd.h>

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
    CHECK(!parseDecl("float width 600 0 512"));      // default out of range
    CHECK(!parseDecl("float ht_width 1"));           // reserved prefix
    CHECK(!parseDecl("float Width 1"));              // uppercase first
    CHECK(!parseDecl("colour c 0xff000000"));        // unknown type
    CHECK(!parseDecl("color c rgba(ff0000ff) 0 1")); // colors take no range
    CHECK(!parseDecl("float a"));                    // missing default

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
    CHECK(!CExpr::parse("-1"));        // no unary minus
    CHECK(!CExpr::parse("max(1, 2)")); // no functions
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
        CHECK(v->text.contains("ht_a_p0Pos")); // path prelude injected
        CHECK(v->sourceNames.size() == 2);     // main + prelude
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

    // Instanced topology (SPEC §13.3): K is a literal 1..64 or a param name.
    const std::string instHead = "#version 300 es\n#pragma hyprtail contract 2\n";
    const auto        inst     = [&](const std::string& topology, const std::string& rest = "") {
        return pp(instHead + "#pragma hyprtail topology " + topology + "\n" + rest + "void main() {}\n", eStage::VERTEX);
    };

    auto i8 = inst("instanced 8");
    CHECK(i8 && i8->topology == eTopology::INSTANCED && i8->instances.literal == 8 && i8->instances.param.empty());
    CHECK(i8 && i8->text.contains("ht_a_pos") && i8->text.contains("ht_instance") && i8->text.contains("uniform int ht_K;") && !i8->text.contains("ht_a_p0Pos"));
    CHECK(inst("instanced 1") && inst("instanced 64"));
    CHECK(!inst("instanced 0") && !inst("instanced 65") && !inst("instanced 99999999999999999999")); // outside 1..64
    CHECK(!inst("instanced"));                                                                       // K is required
    CHECK(!inst("instanced -1") && !inst("instanced 1.5") && !inst("instanced Copies"));             // neither a count nor a param name
    CHECK(!inst("instanced 8 9"));
    CHECK(!inst("path 4") && !inst("quad 1")); // no options
    auto ip = inst("instanced copies", "#pragma hyprtail param int copies 4 1 64\n");
    CHECK(ip && ip->instances.param == "copies" && ip->instances.literal == 0);
    CHECK(shader::topologyText(eTopology::INSTANCED, i8->instances) == "instanced 8");
    CHECK(ip && shader::topologyText(eTopology::INSTANCED, ip->instances) == "instanced copies");
    CHECK(shader::topologyText(eTopology::PATH, {}) == "path" && shader::topologyText(eTopology::QUAD, {}) == "quad");

    // K param: declared, int, with a range inside 1..64.
    const auto kProblem = [&](const std::string& decl) {
        auto                       v = inst("instanced copies", decl);
        std::vector<params::SDecl> declared;
        if (v)
            for (const auto& p : v->params)
                declared.push_back(p.decl);
        return v ? shader::instanceCountProblem(*v, declared) : std::optional<std::string>{"preprocess failed"};
    };
    CHECK(!kProblem("#pragma hyprtail param int copies 4 1 64\n"));
    CHECK(!kProblem("#pragma hyprtail param int copies 4 2 16\n"));
    CHECK(kProblem(""));                                             // not declared
    CHECK(kProblem("#pragma hyprtail param float copies 4 1 64\n")); // not an int
    CHECK(kProblem("#pragma hyprtail param int copies 4\n"));        // no range
    CHECK(kProblem("#pragma hyprtail param int copies 4 0 64\n"));   // min below 1
    CHECK(kProblem("#pragma hyprtail param int copies 4 1 65\n"));   // max above 64
    CHECK(!shader::instanceCountProblem(*i8, {}));                   // a literal needs no param

    // expects: kinds path, quad, instanced; checked against the vertex shader.
    const auto frag = [&](const std::string& expects) { return pp(instHead + "#pragma hyprtail expects " + expects + "\nvoid main() {}\n", eStage::FRAGMENT); };
    CHECK(frag("instanced") && frag("quad,instanced") && frag("path,quad,instanced"));
    CHECK(!frag("instanced 8") && !frag("particles"));
    const auto fInst = frag("quad,instanced"), fPath = frag("path");
    CHECK(fInst && fPath && i8 && ip);
    if (fInst && fPath && i8 && ip) {
        CHECK(!shader::expectsMismatch(*i8, *fInst) && !shader::expectsMismatch(*ip, *fInst));
        const auto bad = shader::expectsMismatch(*i8, *fPath);
        CHECK(bad && bad->contains("expects topology path") && bad->contains("declares instanced 8"));
        const auto badParam = shader::expectsMismatch(*ip, *fPath);
        CHECK(badParam && badParam->contains("declares instanced copies"));
        const auto quad = pp(instHead + "#pragma hyprtail topology quad\nvoid main() {}\n", eStage::VERTEX);
        CHECK(quad && shader::expectsMismatch(*quad, *fInst).has_value() == false);
        CHECK(quad && shader::expectsMismatch(*quad, *fPath).has_value());
    }
    CHECK(!shader::expectsMismatch(*i8, *pp(instHead + "void main() {}\n", eStage::FRAGMENT))); // no expects: any topology

    // The prelude's attributes are exactly the locations the loader says it
    // feeds (the program contract check refuses anything else).
    for (const auto t : {eTopology::PATH, eTopology::INSTANCED}) {
        const auto       src = pp(instHead + "#pragma hyprtail topology " + (t == eTopology::PATH ? "path" : "instanced 2") + "\nvoid main() {}\n", eStage::VERTEX);
        std::vector<int> found;
        if (src) {
            static const std::regex RE{R"(layout\(location = (\d+)\) in )"};
            for (auto it = std::sregex_iterator(src->text.begin(), src->text.end(), RE); it != std::sregex_iterator(); ++it)
                found.push_back(std::stoi((*it)[1]));
        }
        std::ranges::sort(found);
        CHECK(!found.empty() && found == shader::preludeAttribLocations(t));
    }
    CHECK(shader::preludeAttribLocations(eTopology::INSTANCED).size() == 5 && shader::preludeAttribLocations(eTopology::QUAD).empty());
    CHECK(std::ranges::find(shader::preludeUniforms(), std::string{"ht_K"}) != shader::preludeUniforms().end());

    // Reserved and duplicate params, bad padding, unknown pragma.
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail param float fade_ms 1\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail param float a 1\n#pragma hyprtail param float a 1\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail padding max(1, 2)\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail glow 1\nvoid main() {}\n", eStage::FRAGMENT));

    // Built-ins preprocess.
    for (const auto* name : {"shaders/ribbon.vert", "shaders/scatter.vert", "shaders/drift.vert", "shaders/halo.vert"})
        CHECK(shader::preprocess(shader::builtin(name), name, {}, eStage::VERTEX).has_value());
    for (const auto* name : {"shaders/gradient.frag", "shaders/dots.frag", "shaders/pulse.frag", "shaders/sizzle.frag"})
        CHECK(shader::preprocess(shader::builtin(name), name, {}, eStage::FRAGMENT).has_value());

    // The instanced built-ins declare what the loader checks: topology,
    // a K param inside 1..64, a padding expression, and (dots.frag) expects.
    for (const auto* name : {"shaders/scatter.vert", "shaders/drift.vert"}) {
        const auto s = shader::preprocess(shader::builtin(name), name, {}, eStage::VERTEX);
        CHECK(s && s->topology == eTopology::INSTANCED && !s->instances.param.empty() && !s->padding.empty());
        if (!s)
            continue;
        std::vector<params::SDecl> declared;
        for (const auto& p : s->params)
            declared.push_back(p.decl);
        CHECK(!shader::instanceCountProblem(*s, declared));
    }
    const auto dots = shader::preprocess(shader::builtin("shaders/dots.frag"), "shaders/dots.frag", {}, eStage::FRAGMENT);
    CHECK(dots && dots->expects.size() == 2);

    // The shared palette (helpers/palette.glsl): every look that colors with
    // it declares its params with the same names, types and ranges, so a
    // preset's colors carry over when a layer swaps looks.
    const auto gradient = shader::preprocess(shader::builtin("shaders/gradient.frag"), "shaders/gradient.frag", {}, eStage::FRAGMENT);
    CHECK(gradient.has_value());
    if (gradient && dots) {
        for (const auto* name : {"color_a", "color_b", "color_by", "speed_ref", "color_period"}) {
            const auto find = [&](const shader::SSource& s) { return std::ranges::find_if(s.params, [&](const auto& p) { return p.decl.name == name; }); };
            const auto g = find(*gradient), d = find(*dots);
            CHECK(g != gradient->params.end() && d != dots->params.end());
            if (g != gradient->params.end() && d != dots->params.end())
                CHECK(g->decl.type == d->decl.type && g->decl.min == d->decl.min && g->decl.max == d->decl.max);
        }
    }
}

// The built-in preset manifests (hyprtail/presets/*.conf, run from the repo root):
// every layer names built-in shaders that preprocess, and every other key
// is a parameter of the paired program (or a reserved one) with a value of
// its type inside its range. preset::parse() itself needs Hyprland headers,
// so this reads the files with the same "key = value" / "layer:name" rules;
// it's the part that would otherwise only show up as a runtime warning.
static void testPresetManifests() {
    namespace fs = std::filesystem;
    if (!fs::is_directory("hyprtail/presets"))
        return;

    const auto trim = [](std::string s) {
        const auto ws = " \t\r";
        s.erase(0, s.find_first_not_of(ws));
        s.erase(s.find_last_not_of(ws) + 1);
        return s;
    };

    using Layers = std::map<std::string, std::map<std::string, std::string>>;

    int                                manifests = 0;
    std::map<std::string, Layers>      byPreset;   // file stem -> layer -> key -> value
    std::map<std::string, std::string> layerOrder; // file stem -> its "layers = ..." value
    std::map<std::string, std::string> sourceOf;   // file stem -> its "source = ..." value, if any
    for (const auto& entry : fs::directory_iterator("hyprtail/presets")) {
        if (entry.path().extension() != ".conf")
            continue;
        ++manifests;

        Layers        layers;
        std::ifstream in(entry.path());
        std::string   line;
        while (std::getline(in, line)) {
            line          = trim(line.substr(0, line.find('#')));
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const auto key   = trim(line.substr(0, eq));
            const auto colon = key.find(':');
            if (colon != std::string::npos)
                layers[trim(key.substr(0, colon))][trim(key.substr(colon + 1))] = trim(line.substr(eq + 1));
            else if (key == "layers")
                layerOrder[entry.path().stem().string()] = trim(line.substr(eq + 1));
            else if (key == "source") {
                sourceOf[entry.path().stem().string()] = trim(line.substr(eq + 1));
                CHECK(source::known(sourceOf[entry.path().stem().string()]));
            }
        }
        CHECK(!layers.empty());
        byPreset[entry.path().stem().string()] = layers;

        for (const auto& [layer, keys] : layers) {
            const auto fail = [&](const std::string& why) {
                std::cerr << std::format("preset {} layer {}: {}\n", entry.path().string(), layer, why);
                CHECK(false);
            };

            // "source:<name>": a setting of the preset's source, not a layer.
            if (layer == source::KEY_PREFIX) {
                const auto  stem  = entry.path().stem().string();
                const auto  kind  = sourceOf.contains(stem) ? sourceOf[stem] : std::string{source::DEFAULT_KIND};
                const auto& decls = source::decls(kind);
                for (const auto& [name, text] : keys) {
                    const auto decl = std::ranges::find_if(decls, [&](const auto& d) { return d.name == name; });
                    if (decl == decls.end()) {
                        fail(std::format("\"{}\" isn't a setting of source {}", name, kind));
                        continue;
                    }
                    auto v = params::parseValue(decl->type, text);
                    if (v)
                        if (auto r = params::checkRange(*decl, *v); !r)
                            v = std::unexpected(r.error());
                    if (!v)
                        fail(std::format("{} = {}: {}", name, text, v.error()));
                }
                continue;
            }

            const auto vertKey = keys.find("vertex"), fragKey = keys.find("fragment");
            // Shader references are paths relative to the hyprtail root
            // (shaders/ribbon.vert), the same string in both modes. Embedded
            // mode: it is a key of shader::builtin(), never read from disk.
            // Disk mode: it is a file under hyprtail/ (this repo's copy of
            // the root) holding the same text as the embedded one, so a
            // copied folder and the prefab: preset draw the same thing.
            if (vertKey == keys.end() || fragKey == keys.end() || vertKey->second.starts_with("prefab:") || fragKey->second.starts_with("prefab:")) {
                fail("needs vertex and fragment shader paths relative to the hyprtail root (shaders/ribbon.vert), not prefab:");
                continue;
            }
            const auto vertName = vertKey->second, fragName = fragKey->second;
            bool       pathsOk  = true;
            for (const auto& name : {vertName, fragName}) {
                const auto embedded = shader::builtin(name);
                if (embedded.empty()) {
                    fail(std::format("{} isn't in the embedded shader table", name));
                    pathsOk = false;
                    continue;
                }
                std::ifstream     in(fs::path{"hyprtail"} / name, std::ios::binary);
                std::stringstream disk;
                disk << in.rdbuf();
                if (!in) {
                    fail(std::format("hyprtail/{} doesn't exist on disk", name));
                    pathsOk = false;
                } else if (disk.str() != embedded) {
                    fail(std::format("hyprtail/{} on disk differs from the embedded copy", name));
                    pathsOk = false;
                }
            }
            if (!pathsOk)
                continue;
            const auto vert = shader::preprocess(shader::builtin(vertName), vertName, {}, shader::eStage::VERTEX);
            const auto frag = shader::preprocess(shader::builtin(fragName), fragName, {}, shader::eStage::FRAGMENT);
            if (!vert || !frag) {
                fail(std::format("{} or {} doesn't preprocess", vertName, fragName));
                continue;
            }
            if (const auto mismatch = shader::expectsMismatch(*vert, *frag))
                fail(*mismatch);

            std::vector<params::SDecl> decls;
            for (const auto& r : shader::reservedParams())
                decls.push_back(r.decl);
            for (const auto* src : {&*vert, &*frag})
                for (const auto& p : src->params)
                    decls.push_back(p.decl);
            if (const auto problem = shader::instanceCountProblem(*vert, decls))
                fail(*problem);

            for (const auto& [name, text] : keys) {
                if (name == "vertex" || name == "fragment")
                    continue;
                const auto decl = std::ranges::find_if(decls, [&](const auto& d) { return d.name == name; });
                if (decl == decls.end()) {
                    fail(std::format("\"{}\" isn't a parameter of {} + {}", name, vertName, fragName));
                    continue;
                }
                auto v = params::parseValue(decl->type, text);
                if (v)
                    if (auto r = params::checkRange(*decl, *v); !r)
                        v = std::unexpected(r.error());
                if (!v)
                    fail(std::format("{} = {}: {}", name, text, v.error()));
            }
        }
    }
    CHECK(manifests >= 6); // jitter, vivid, comet, embers, spring, ink

    // The shipped presets that are built purely from other shipped parts:
    // each exists, lists the layers it should, and pairs the shaders it should.
    // (That every layer's shaders, keys and values are valid is the loop above.)
    const auto val = [&](const std::string& preset, const std::string& layer, const std::string& key) -> std::string {
        const auto p = byPreset.find(preset);
        if (p == byPreset.end() || !p->second.contains(layer) || !p->second.at(layer).contains(key))
            return {};
        return p->second.at(layer).at(key);
    };
    const auto num   = [&](const std::string& preset, const std::string& layer, const std::string& key) { return std::strtod(val(preset, layer, key).c_str(), nullptr); };
    const auto alpha = [&](const std::string& preset, const std::string& layer, const std::string& key) {
        const auto c = params::parseValue(params::eType::COLOR, val(preset, layer, key));
        return c ? static_cast<int>(c->argb >> 24) : -1;
    };
    const auto ribbon = [&](const std::string& preset, const std::string& layer) {
        return val(preset, layer, "vertex") == "shaders/ribbon.vert" && val(preset, layer, "fragment") == "shaders/gradient.frag";
    };

    // spring: one ribbon layer over the spring source, with its settings
    // set; every other shipped preset leaves the source at the pointer's.
    CHECK(sourceOf["spring"] == "spring" && layerOrder["spring"] == "trail" && ribbon("spring", "trail"));
    CHECK(num("spring", "source", "stiffness") > 0.0 && num("spring", "source", "damping") > 0.0 && num("spring", "source", "age_step_ms") > 0.0);
    for (const auto& [stem, kind] : sourceOf)
        CHECK(stem == "spring" || kind == "pointer");

    // vivid: a wide faint glow under a narrow opaque core, same ribbon shaders.
    CHECK(layerOrder["vivid"] == "glow, core");
    CHECK(ribbon("vivid", "glow") && ribbon("vivid", "core"));
    CHECK(num("vivid", "glow", "width") > num("vivid", "core", "width") && num("vivid", "core", "width") > 0.0);
    for (const auto* key : {"color_a", "color_b"}) {
        CHECK(alpha("vivid", "glow", key) > 0 && alpha("vivid", "glow", key) < 0x80);
        CHECK(alpha("vivid", "core", key) == 0xff);
    }

    // ink: one ribbon drawn with a flat nib, colored by life, plus an idle
    // ring that ships disabled.
    CHECK(layerOrder["ink"] == "ink, idle" && ribbon("ink", "ink"));
    CHECK(val("ink", "idle", "vertex") == "shaders/halo.vert" && val("ink", "idle", "fragment") == "shaders/pulse.frag" && val("ink", "idle", "enabled") == "false");
    CHECK(num("ink", "ink", "nib") > 0.0 && num("ink", "ink", "color_by") == 1.0);

    // comet: sparks under a narrow ribbon tail with a short fade and a higher
    // speed_ref than the ribbon default (2).
    CHECK(layerOrder["comet"] == "sparks, tail" && ribbon("comet", "tail"));
    CHECK(val("comet", "sparks", "vertex") == "shaders/drift.vert" && val("comet", "sparks", "fragment") == "shaders/dots.frag");
    CHECK(num("comet", "tail", "width") > 0.0 && num("comet", "tail", "width") < num("ink", "ink", "width"));
    CHECK(num("comet", "tail", "fade_ms") < num("ink", "ink", "fade_ms"));
    CHECK(num("comet", "tail", "speed_ref") > 2.0);

    // embers: rising drift particles plus an idle crackle, and its own count,
    // speed and fade rather than a copy of another particle preset's.
    CHECK(layerOrder["embers"] == "embers, crackle");
    CHECK(val("embers", "embers", "vertex") == "shaders/drift.vert" && val("embers", "embers", "fragment") == "shaders/dots.frag");
    CHECK(val("embers", "crackle", "vertex") == "shaders/halo.vert" && val("embers", "crackle", "fragment") == "shaders/sizzle.frag");
    CHECK(num("embers", "embers", "gravity") > 0.0 && val("embers", "embers", "gravity_dir") == "0,-1");

    // Reworked presets name their layers uniquely, so a layer block pasted
    // from one into another never collides.
    {
        std::map<std::string, int> uses;
        for (const auto* stem : {"ink", "comet", "embers"})
            for (const auto& [layer, keys] : byPreset[stem])
                ++uses[layer];
        for (const auto& [layer, n] : uses)
            CHECK(n == 1);
    }
}

static void testCrashGuard() {
    using namespace hyprtail::crashguard;

    // parse: round-trips through format(), and rejects malformed/incomplete
    // text; unknown extra fields are tolerated.
    const SKey key{"abc1234", "deadbeef"};
    const auto text = format(key, "sig-1", 4242);
    const auto m    = parse(text);
    CHECK(m && m->revision == key.revision && m->hyprlandHash == key.hyprlandHash && m->instanceSignature == "sig-1" && m->pid == 4242);

    CHECK(!parse(""));
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\n"));          // missing pid
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\npid=abc\n")); // pid not a number
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\npid=-1\n"));  // pid not positive
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\npid=0\n"));
    CHECK(parse("rev=a\nhyprland=b\ninstance=c\npid=5\nextra=ignored\n").has_value());

    // Stale pid detection: a reaped child's pid is guaranteed dead; our own
    // pid is guaranteed alive.
    const pid_t child = fork();
    if (child == 0)
        _exit(0);
    CHECK(child > 0);
    int status = 0;
    if (child > 0)
        waitpid(child, &status, 0);
    const long long deadPid = child;
    const long long livePid = static_cast<long long>(::getpid());

    CHECK(pidIsDead(deadPid));
    CHECK(!pidIsDead(livePid));
    CHECK(!pidIsDead(0));
    CHECK(!pidIsDead(-1));

    // Key match: only revision+hash+dead-pid together indicate an early
    // death; any single mismatch keeps the load going.
    const SMarker deadSameKey{key.revision, key.hyprlandHash, "sig-1", deadPid};
    const SMarker liveSameKey{key.revision, key.hyprlandHash, "sig-1", livePid};
    const SMarker deadOtherRev{"different", key.hyprlandHash, "sig-1", deadPid};
    const SMarker deadOtherHash{key.revision, "different", "sig-1", deadPid};

    CHECK(indicatesEarlyDeath(deadSameKey, key));
    CHECK(!indicatesEarlyDeath(liveSameKey, key));   // live pid: another instance, not a crash
    CHECK(!indicatesEarlyDeath(deadOtherRev, key));  // different build
    CHECK(!indicatesEarlyDeath(deadOtherHash, key)); // different running Hyprland

    // Real file round trip, redirected to a scratch dir so this never
    // touches the real state directory.
    char tmpl[] = "/tmp/hyprtail-unit-XXXXXX";
    if (const char* dir = mkdtemp(tmpl)) {
        setenv("XDG_STATE_HOME", dir, 1);
        CHECK(!readMarker()); // nothing written yet
        CHECK(writeMarker(key, "sig-1", livePid));
        const auto readBack = readMarker();
        CHECK(readBack && readBack->revision == key.revision && readBack->pid == livePid);
        removeMarker();
        CHECK(!readMarker());
        std::filesystem::remove_all(std::filesystem::path{dir});
    } else
        CHECK(false); // mkdtemp failing is an environment problem worth flagging
}

// Per-app suppression state (SPEC section 7): the enter and exit transitions of
// CPointerGate and what they do to the point buffer, for both sources.
static void testPointerGate() {
    for (const char* kind : {"pointer", "spring"}) {
        auto                  src = source::make(kind, 8, 7);
        CPointerGate          gate;
        const SVec2f          a{10, 10}, b{20, 10}, c{500, 400}, d{510, 400};
        std::vector<SGpuNode> out;

        src->insert(a, 0.0, false);
        src->insert(b, 10.0, false);

        // Not excluded and staying so: no edge, nothing touched.
        const auto gen = src->generation();
        const auto n   = src->size();
        CHECK(n > 0);
        CHECK(!gate.excluded());
        CHECK(gate.update(false, *src) == eGateEdge::NONE);
        CHECK(src->size() == n && src->generation() == gen);

        // Enter: the buffer is dropped, nothing stays to be drawn.
        CHECK(gate.update(true, *src) == eGateEdge::ENTER);
        CHECK(gate.excluded());
        CHECK(src->empty() && src->generation() != gen);
        CHECK(!src->visibleBounds(10.0, 1000.0));

        // Excluded and staying so: no edge, and the buffer is left alone
        // (the caller inserts nothing, but update() must not churn it).
        const auto emptyGen = src->generation();
        CHECK(gate.update(true, *src) == eGateEdge::NONE);
        CHECK(gate.excluded() && src->empty() && src->generation() == emptyGen);

        // Exit: an empty buffer, so the first new node is joined to nothing
        // from before the excluded window.
        CHECK(gate.update(false, *src) == eGateEdge::EXIT);
        CHECK(!gate.excluded() && src->empty());
        src->insert(c, 500.0, false);
        src->orderedCopy(out, 500.0);
        CHECK(!out.empty() && (out[0].bits & GPU_BIT_SEGMENT_START));
        CHECK(std::ranges::all_of(out, [&](const SGpuNode& node) { return node.posPx == c; })); // nothing from before

        // Back to normal: a not-excluded update keeps what was inserted.
        src->insert(d, 510.0, false);
        const auto before = src->generation();
        const auto m      = src->size();
        CHECK(gate.update(false, *src) == eGateEdge::NONE);
        CHECK(src->size() == m && src->generation() == before);

        // Straight from exit to enter again, and an enter on an empty buffer.
        CHECK(gate.update(true, *src) == eGateEdge::ENTER && src->empty());
        CHECK(gate.update(false, *src) == eGateEdge::EXIT && src->empty());
        CHECK(gate.update(true, *src) == eGateEdge::ENTER && src->empty());
    }
}

static void testRing() {
    CTrailRing ring(8, 42);
    ring.insert({0, 0}, 0.0, false);     // first node: segment start
    ring.insert({3, 4}, 10.0, false);    // +5
    ring.insert({6, 8}, 20.0, false);    // +5
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

    // Visible range (SPEC §13.3): the newest nodes with age < fade. Nodes
    // are at t = 0, 10, 20, 30 (a segment start), 40; now = 45.
    CHECK(ring.visibleCount(45.0, 100.0) == 5);
    CHECK(ring.visibleCount(45.0, 30.0) == 3); // ages 5, 15, 25 (35 is faded)
    CHECK(ring.visibleCount(45.0, 25.0) == 2); // age 25 is not < 25
    CHECK(ring.visibleCount(45.0, 4.0) == 0);
    CHECK(CTrailRing(4, 1).visibleCount(0.0, 100.0) == 0); // empty

    // Bounds: nodes at (6,8), (100,100), (100,110) are visible at fade 30.
    // Path layers add the next older node (3,4), connected to (6,8); an
    // instanced layer draws visible nodes only.
    const auto withOlder = ring.visibleBounds(45.0, 30.0);
    const auto nodesOnly = ring.visibleBounds(45.0, 30.0, false);
    CHECK(withOlder && withOlder->x1 == 3.F && withOlder->y1 == 4.F && withOlder->x2 == 100.F && withOlder->y2 == 110.F);
    CHECK(nodesOnly && nodesOnly->x1 == 6.F && nodesOnly->y1 == 8.F && nodesOnly->x2 == 100.F && nodesOnly->y2 == 110.F);
    // Fade 20: (100,100) and (100,110) visible; the older node (6,8) is
    // separated by the segment start, so neither variant includes it.
    const auto brk     = ring.visibleBounds(45.0, 20.0);
    const auto brkOnly = ring.visibleBounds(45.0, 20.0, false);
    CHECK(brk && brkOnly && brk->x1 == 100.F && brk->y1 == 100.F && brkOnly->x1 == 100.F && brkOnly->y1 == 100.F);
    CHECK(!ring.visibleBounds(45.0, 4.0, false));

    // After the ring wraps (capacity 4, six inserts keep t = 20..50) and
    // after a resize.
    CTrailRing wrapped(4, 7);
    for (int i = 0; i <= 5; ++i)
        wrapped.insert({static_cast<float>(i), 0.F}, i * 10.0, false);
    CHECK(wrapped.size() == 4 && wrapped.visibleCount(55.0, 100.0) == 4);
    CHECK(wrapped.visibleCount(55.0, 25.0) == 2); // ages 5, 15
    const auto wb = wrapped.visibleBounds(55.0, 25.0, false);
    CHECK(wb && wb->x1 == 4.F && wb->x2 == 5.F);
    wrapped.resize(2);
    CHECK(wrapped.size() == 2 && wrapped.visibleCount(55.0, 100.0) == 2 && wrapped.visibleCount(55.0, 10.0) == 1);
}

// A source that changes between inserts: stands in for a future animated one
// to exercise the continuous-upload half of the gate.
namespace {
    struct SContinuousStub final : ISource {
        std::string_view kind() const override {
            return "stub";
        }
        void   insert(const SVec2f&, double, bool) override {}
        void   tick(double, double) override {}
        void   configure(const std::map<std::string, double>&) override {}
        void   resize(size_t) override {}
        void   clear() override {}
        size_t size() const override {
            return 0;
        }
        size_t capacity() const override {
            return 0;
        }
        const SCursorNode& newest() const override {
            return node_;
        }
        void orderedCopy(std::vector<SGpuNode>&, double) const override {}
        bool needsContinuousUpload() const override {
            return true;
        }
        size_t visibleCount(double, double) const override {
            return 0;
        }
        std::optional<STrailBounds> visibleBounds(double, double, bool) const override {
            return std::nullopt;
        }
        bool isSettled(double, double) const override {
            return false;
        }
        uint64_t generation() const override {
            return 7;
        }
        bool empty() const override {
            return empty_;
        }
        double newestBirthMs() const override {
            return 0.0;
        }
        bool        empty_ = false;
        SCursorNode node_{};
    };
}

static void testSource() {
    CTrailRing ring(8, 42);

    // The real-history ring never asks for continuous upload, and is settled
    // exactly when nothing is visible.
    CHECK(!ring.needsContinuousUpload());
    CHECK(ring.isSettled(0.0, 100.0)); // empty
    ring.tick(10.0, 10.0);             // no-op
    CHECK(ring.empty() && ring.generation() == 0);

    ring.insert({0, 0}, 0.0, false);
    ring.insert({3, 4}, 10.0, false);
    ring.insert({6, 8}, 20.0, false);
    CHECK(!ring.needsContinuousUpload());
    CHECK(ring.newestBirthMs() == 20.0);
    for (const double now : {0.0, 20.0, 25.0, 45.0, 1000.0}) {
        for (const double fade : {1.0, 10.0, 30.0, 100.0})
            CHECK(ring.isSettled(now, fade) == (ring.visibleCount(now, fade) == 0));
    }
    CHECK(!ring.isSettled(25.0, 30.0) && ring.isSettled(1000.0, 30.0));

    // Upload gate: for the ring, identical to the condition it replaced,
    // upload unless empty or the generation is unchanged.
    const auto old  = [](const CTrailRing& r, uint64_t uploaded) { return !(r.empty() || r.generation() == uploaded); };
    const auto same = [&](const CTrailRing& r, uint64_t uploaded) { return sourceNeedsUpload(r, uploaded) == old(r, uploaded); };

    CTrailRing fresh(4, 1);
    CHECK(same(fresh, UINT64_MAX) && !sourceNeedsUpload(fresh, UINT64_MAX));             // empty: nothing to upload
    CHECK(same(ring, UINT64_MAX) && sourceNeedsUpload(ring, UINT64_MAX));                // never uploaded
    CHECK(same(ring, ring.generation()) && !sourceNeedsUpload(ring, ring.generation())); // up to date
    const auto uploaded = ring.generation();
    ring.insert({9, 12}, 30.0, false);
    CHECK(same(ring, uploaded) && sourceNeedsUpload(ring, uploaded)); // insert
    const auto afterInsert = ring.generation();
    ring.resize(2);
    CHECK(same(ring, afterInsert) && sourceNeedsUpload(ring, afterInsert)); // resize
    const auto afterResize = ring.generation();
    ring.clear();
    CHECK(same(ring, afterResize) && !sourceNeedsUpload(ring, afterResize)); // clear: empty, nothing to upload

    // A continuous source uploads every frame even at an unchanged
    // generation, but never when empty.
    SContinuousStub stub;
    CHECK(sourceNeedsUpload(stub, stub.generation()));
    stub.empty_ = true;
    CHECK(!sourceNeedsUpload(stub, stub.generation()));
}

// What the renderer does each frame with a source (CNodeBuffer::upload):
// upload if the gate says so, remembering what was uploaded.
namespace {
    struct SUploadSim {
        std::vector<SGpuNode> uploaded;
        uint64_t              gen     = UINT64_MAX;
        double                ref     = 0.0;
        int                   uploads = 0;

        void                  frame(const ISource& s) {
            if (!sourceNeedsUpload(s, gen))
                return;
            ref = s.newestBirthMs();
            s.orderedCopy(uploaded, ref);
            gen = s.generation();
            ++uploads;
        }

        // The GPU holds what the source would upload right now.
        bool current(const ISource& s) const {
            std::vector<SGpuNode> now;
            s.orderedCopy(now, ref);
            return ref == s.newestBirthMs() && now.size() == uploaded.size() && std::memcmp(now.data(), uploaded.data(), now.size() * sizeof(SGpuNode)) == 0;
        }
    };

    std::vector<SGpuNode> copyOf(const ISource& s) {
        std::vector<SGpuNode> out;
        s.orderedCopy(out, s.newestBirthMs());
        return out;
    }
}

static void testSpringChain() {
    // The reframing (value = 1 + pos - target, pos = target + value - 1)
    // against an independent answer: critically damped (omega 50 rad/s, mass
    // 1, stiffness omega^2, damping 2 omega) from 100 px away at rest has
    // displacement d0 (1 + wt) e^-wt and velocity -d0 w^2 t e^-wt.
    {
        CSpringChainSource c(1, 1);
        c.configure({{"mass", 1}, {"stiffness", 2500}, {"damping", 100}});
        CHECK(c.empty() && c.size() == 0 && !c.needsContinuousUpload()); // NOLINT(readability-container-size-empty): asserts size() agrees with empty()
        c.insert({0, 0}, 0.0, false);                                    // seeds the chain
        CHECK(!c.empty() && c.size() == 1 && !c.needsContinuousUpload());
        c.insert({100, 0}, 0.0, false);
        CHECK(c.needsContinuousUpload());
        c.tick(16.0, 16.0);
        const auto out = copyOf(c);
        CHECK(out.size() == 1 && std::abs(out[0].posPx.x - 19.121F) < 0.05F && out[0].posPx.y == 0.F);
        CHECK(std::abs(out[0].velocity.x - 1.797F) < 0.01F); // px/ms
    }

    // dt 0 moves nothing, and a long gap integrates at most MAX_DT (33 ms).
    {
        CSpringChainSource a(4, 1), b(4, 1), c(4, 1);
        for (auto* s : {&a, &b, &c}) {
            s->configure({{"mass", 1}, {"stiffness", 400}, {"damping", 60}}); // slow: 33 ms is far from arriving
            s->insert({0, 0}, 0.0, false);
            s->insert({100, 40}, 0.0, false);
        }
        const auto before = copyOf(c);
        c.tick(5.0, 0.0);
        const auto after = copyOf(c);
        CHECK(before.size() == after.size() && std::memcmp(before.data(), after.data(), before.size() * sizeof(SGpuNode)) == 0 && c.needsContinuousUpload());
        a.tick(10000.0, 10000.0);
        b.tick(10000.0, 33.0);
        const auto ca = copyOf(a), cb = copyOf(b);
        CHECK(ca.size() == cb.size() && ca.back().posPx == cb.back().posPx && ca.back().posPx.x > 0.F && ca.back().posPx.x < 90.F);
    }

    // Overdamped: every point trails the one before it and none overshoots.
    {
        CSpringChainSource c(6, 1);
        c.configure({{"mass", 1}, {"stiffness", 4000}, {"damping", 400}});
        c.insert({0, 0}, 0.0, false);
        c.insert({300, 0}, 0.0, false);
        for (int i = 1; i <= 5; ++i)
            c.tick(i * 16.0, 16.0);
        const auto out     = copyOf(c); // tail first
        bool       ordered = out.size() == 6, inside = true;
        for (size_t i = 0; i + 1 < out.size(); ++i)
            ordered = ordered && out[i].posPx.x <= out[i + 1].posPx.x;
        for (const auto& n : out)
            inside = inside && n.posPx.x >= 0.F && n.posPx.x <= 300.F;
        CHECK(ordered && inside && out.back().posPx.x > out.front().posPx.x);
    }

    // Moves, settles exactly on the target, and what the renderer uploaded
    // under the upload gate is the final state, however many frames it took.
    {
        CSpringChainSource c(8, 7);
        SUploadSim         sim;
        c.insert({0, 0}, 0.0, false);
        sim.frame(c);
        c.insert({200, 50}, 10.0, false);
        double now    = 10.0;
        int    frames = 0;
        for (; frames < 2000; ++frames) {
            now += 16.0;
            c.tick(now, 16.0);
            sim.frame(c);
            if (!c.needsContinuousUpload())
                break;
        }
        CHECK(frames < 2000);
        CHECK(sim.current(c));
        bool atTarget = true;
        for (const auto& n : copyOf(c))
            atTarget = atTarget && n.posPx == SVec2f{200.F, 50.F} && n.velocity == SVec2f{};
        CHECK(atTarget);

        // Settled and unchanged: no more uploads, no more generations.
        const auto gen     = c.generation();
        const int  uploads = sim.uploads;
        for (int i = 0; i < 10; ++i) {
            now += 16.0;
            c.tick(now, 16.0);
            sim.frame(c);
        }
        CHECK(c.generation() == gen && sim.uploads == uploads && sim.current(c));

        // The fade check: stopped is not settled until it has faded too.
        const double active = c.newestBirthMs();
        CHECK(!c.isSettled(active + 1.0, 100.0));
        CHECK(c.isSettled(active + 101.0, 100.0));
    }

    // Births: point k is (activeMs - k * age_step_ms); the tail is oldest and
    // the only segment start; distances grow from the tail; seeds are fixed.
    {
        CSpringChainSource c(4, 9);
        c.insert({0, 0}, 100.0, false);
        auto out = copyOf(c);
        CHECK(out.size() == 4 && out[3].birthMs == 0.F && out[2].birthMs == -10.F && out[0].birthMs == -30.F);
        CHECK((out[0].bits & GPU_BIT_SEGMENT_START) && !(out[1].bits & GPU_BIT_SEGMENT_START) && !(out[3].bits & GPU_BIT_SEGMENT_START));
        CHECK(c.newestBirthMs() == 100.0 && c.newest().posPx == SVec2f{} && c.newest().birthTimeMs == 100.0);
        CHECK((out[0].bits >> 1) != (out[1].bits >> 1));

        c.insert({30, 40}, 110.0, false);
        for (int i = 1; i <= 3; ++i)
            c.tick(110.0 + i * 16.0, 16.0);
        const auto moved = copyOf(c);
        CHECK(moved[0].distPx == 0.F && moved[0].distPx <= moved[1].distPx && moved[1].distPx <= moved[2].distPx && moved[2].distPx <= moved[3].distPx && moved[3].distPx > 0.F);
        for (size_t i = 0; i < 4; ++i)
            CHECK((moved[i].bits >> 1) == (out[i].bits >> 1));                        // seeds don't change
        CHECK(c.newest().posPx == SVec2f(30.F, 40.F) && c.newest().velocity.x > 0.F); // the last insert, not the head's position

        // Visibility follows the ages: the newest first, here once settled at t = 110.
        CSpringChainSource s(4, 9);
        s.insert({0, 0}, 100.0, false);
        CHECK(s.visibleCount(100.0, 35.0) == 4 && s.visibleCount(100.0, 25.0) == 3 && s.visibleCount(100.0, 5.0) == 1 && s.visibleCount(200.0, 35.0) == 0);
        CHECK(!s.isSettled(100.0, 35.0) && s.isSettled(200.0, 35.0));
        CHECK(!s.visibleBounds(200.0, 35.0) && s.visibleBounds(100.0, 35.0, false));

        // Unsettled is never settled, faded or not.
        s.insert({50, 0}, 100.0, false);
        CHECK(!s.isSettled(10000.0, 35.0));

        // age_step_ms is live; names the source doesn't have are ignored.
        const auto gen = s.generation();
        s.configure({{"bogus", 1.0}});
        CHECK(s.generation() == gen);
        s.configure({{"age_step_ms", 20.0}});
        CHECK(s.generation() != gen && copyOf(s).front().birthMs == -60.F);
    }

    // A break re-seeds the chain where the pointer is now, at rest.
    {
        CSpringChainSource c(5, 3);
        c.insert({0, 0}, 0.0, false);
        c.insert({200, 0}, 5.0, false);
        c.tick(20.0, 15.0);
        const auto gen = c.generation();
        c.insert({500, 600}, 30.0, true);
        CHECK(c.generation() != gen && !c.needsContinuousUpload() && c.newest().segmentStart);
        bool seeded = true;
        for (const auto& n : copyOf(c))
            seeded = seeded && n.posPx == SVec2f{500.F, 600.F} && n.velocity == SVec2f{};
        CHECK(seeded);
    }

    // resize keeps the head end, clear empties, both bump the generation.
    {
        CSpringChainSource c(4, 3);
        c.resize(6);                                            // not seeded yet: just the capacity
        CHECK(c.capacity() == 6 && c.size() == 0 && c.empty()); // NOLINT(readability-container-size-empty): asserts size() agrees with empty()
        c.insert({10, 20}, 0.0, false);
        c.insert({60, 20}, 5.0, false);
        c.tick(20.0, 15.0);
        const auto head = copyOf(c).back().posPx;
        auto       gen  = c.generation();
        c.resize(6);
        CHECK(c.generation() == gen); // no change
        c.resize(3);
        CHECK(c.size() == 3 && c.capacity() == 3 && c.generation() != gen && copyOf(c).back().posPx == head);
        gen = c.generation();
        c.resize(9);
        CHECK(c.size() == 9 && c.generation() != gen && copyOf(c).back().posPx == head);
        gen = c.generation();
        c.clear();
        // Asserts size() agrees with empty().
        // NOLINTNEXTLINE(readability-container-size-empty)
        CHECK(c.empty() && c.size() == 0 && c.capacity() == 9 && c.generation() != gen && !c.needsContinuousUpload());
        c.tick(100.0, 16.0); // nothing to advance
        c.insert({1, 1}, 100.0, false);
        CHECK(c.size() == 9 && c.newest().posPx == SVec2f(1.F, 1.F));
    }

    // The pointer source satisfies the same contract without moving.
    {
        CTrailRing ring(8, 1);
        CHECK(ring.kind() == "pointer" && ring.capacity() == 8 && ring.empty());
        ring.insert({1, 2}, 5.0, false);
        ring.configure({{"mass", 3.0}}); // nothing to configure
        ring.tick(10.0, 5.0);
        CHECK(ring.size() == 1 && ring.newest().posPx == SVec2f(1.F, 2.F) && ring.newestBirthMs() == 5.0 && !ring.needsContinuousUpload());
    }

    // The factory and the settings a source declares.
    {
        CHECK(source::known("pointer") && source::known("spring") && !source::known("rope") && !source::known(""));
        CHECK(source::decls("pointer").empty() && source::decls("rope").empty());

        const auto& d = source::decls("spring");
        CHECK(d.size() == 4 && d[0].name == "mass" && d[1].name == "stiffness" && d[2].name == "damping" && d[3].name == "age_step_ms");
        CHECK(d.size() == 4 && d[0].def.x == spring::MASS && d[1].def.x == spring::STIFFNESS && d[2].def.x == spring::DAMPING && d[3].def.x == spring::AGE_STEP_MS);

        const auto sp = source::make("spring", 12, 1);
        const auto pt = source::make("pointer", 12, 1);
        CHECK(sp->kind() == "spring" && sp->capacity() == 12 && pt->kind() == "pointer" && pt->capacity() == 12);

        // Declared defaults, then the preset's, then the `params` string's.
        std::string problems;
        auto        r = source::resolve("spring", {{"stiffness", "100"}, {"mass", "2"}}, {{"stiffness", "200"}, {"damping", "-1"}, {"bogus", "1"}}, problems);
        CHECK(r.size() == 4 && r["stiffness"] == 200.0 && r["mass"] == 2.0 && r["damping"] == spring::DAMPING && r["age_step_ms"] == spring::AGE_STEP_MS);
        CHECK(problems.contains("damping") && problems.contains("bogus") && !problems.contains("stiffness") && !problems.contains("mass"));

        problems.clear();
        CHECK(source::resolve("spring", {}, {}, problems).size() == 4 && problems.empty());
        CHECK(source::resolve("pointer", {}, {{"mass", "1"}}, problems).empty() && problems.contains("mass"));

        // A resolved set configures the source it was resolved for.
        CSpringChainSource c(2, 1);
        problems.clear();
        c.configure(source::resolve("spring", {}, {{"age_step_ms", "25"}}, problems));
        c.insert({0, 0}, 50.0, false);
        CHECK(problems.empty() && copyOf(c).front().birthMs == -25.F);
    }
}

// emit_from / emit_offset grammar and bounds (SPEC §13.9, ConfigParse.hpp).
static void testEmitConfig() {
    using namespace hyprtail::cfg;

    // Grammar.
    CHECK(parseTwoFloats("0.5 0.5") == SPair({0.5, 0.5}));
    CHECK(parseTwoFloats("  0   1 ") == SPair({0.0, 1.0}));
    CHECK(!parseTwoFloats("hotspot"));
    CHECK(!parseTwoFloats("0.5"));
    CHECK(!parseTwoFloats("0.5 0.5 0.5"));
    CHECK(!parseTwoFloats("0.5 abc"));
    CHECK(!parseTwoFloats("nan 0"));
    CHECK(!parseTwoFloats("inf 0"));

    // emit_from range: 0 and 1 are in, anything past either edge is out.
    const auto inRange = [](const char* s) {
        const auto p = parseTwoFloats(s);
        return p && emitFromInRange(*p);
    };
    CHECK(inRange("0 0"));
    CHECK(inRange("1 1"));
    CHECK(inRange("0.5 0.25"));
    CHECK(!inRange("1.0001 0.5"));
    CHECK(!inRange("0.5 1.0001"));
    CHECK(!inRange("-0.0001 0.5"));
    CHECK(!inRange("0.5 -0.0001"));
    CHECK(!inRange("2 2"));

    // emit_offset: +-EMIT_OFFSET_MAX_PX per component, inclusive.
    CHECK(emitOffsetInRange(0, 0));
    CHECK(emitOffsetInRange(EMIT_OFFSET_MAX_PX, -EMIT_OFFSET_MAX_PX));
    CHECK(emitOffsetInRange(-3.5, 12));
    CHECK(!emitOffsetInRange(EMIT_OFFSET_MAX_PX + 0.01, 0));
    CHECK(!emitOffsetInRange(0, -EMIT_OFFSET_MAX_PX - 0.01));
    CHECK(!emitOffsetInRange(1e9, 0));
    CHECK(!emitOffsetInRange(std::nan(""), 0));
    CHECK(!emitOffsetInRange(0, INFINITY));

    // First parse: cfg::read() starts from the defaults (s_config = {}), so
    // an out-of-range first value resolves to the default {0, 0}, and a
    // later bad value keeps the last good one.
    const SPair dflt{0.0, 0.0};
    CHECK(resolveEmitOffset(500, 0, dflt) == dflt);
    CHECK(resolveEmitOffset(0, -129, dflt) == dflt);
    CHECK(resolveEmitOffset(std::nan(""), 0, dflt) == dflt);
    CHECK(resolveEmitOffset(5, -7, dflt) == SPair({5.0, -7.0}));
    CHECK(resolveEmitOffset(500, 0, SPair{5.0, -7.0}) == SPair({5.0, -7.0}));
}

// Runs argv with stdout and stderr merged. Returns the exit status (127 if the
// program can't be started) and everything it printed.
static std::pair<int, std::string> runCapture(const std::vector<std::string>& argv) {
    int fds[2];
    if (pipe(fds) != 0)
        return {127, "pipe failed"};

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return {127, "fork failed"};
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> args;
        for (const auto& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }

    close(fds[1]);
    std::string out;
    char        buf[4096];
    ssize_t     n = 0;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, out};
}

namespace {
    // A shader as the loader assembles it, written to a file whose extension
    // tells glslangValidator the stage.
    struct SAssembled {
        std::string     label; // "ribbon.vert", "demo/comet.vert"
        std::string     file;
        shader::SSource src;
    };

    // The shipped shaders are embedded (shader::builtin()); the demo ones
    // (demo/hyprtail/, same layout as hyprtail/) are files, loaded the way a
    // user's own shader is.
    constexpr const char* SHIPPED_SHADERS = "hyprtail/shaders";
    constexpr const char* DEMO_SHADERS    = "demo/hyprtail/shaders";

    // File names in dir with the given extension, sorted (run from the repo root).
    std::vector<std::string> globShaders(const char* dir, std::string_view ext) {
        std::vector<std::string> names;
        if (!std::filesystem::is_directory(dir))
            return names;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().extension() == ext)
                names.push_back(e.path().filename().string());
        }
        std::ranges::sort(names);
        return names;
    }

    // The validator's log for a failed pair, indented, with "<source-id>:<line>"
    // mapped back to file:line through the stage the log is currently about
    // (glslang prints each file's name before its messages).
    std::string readableLog(const std::string& log, const SAssembled& v, const SAssembled& f) {
        const shader::SSource* current = nullptr;
        std::string            out;
        std::istringstream     in{log};
        std::string            line;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            if (line == v.file || line == f.file) {
                current = line == v.file ? &v.src : &f.src;
                continue;
            }
            out += "      " + (current ? shader::mapLog(line, *current) : line) + "\n";
        }
        return out;
    }

    enum class ePair : uint8_t {
        OK,           // links, and the loader accepts the pairing
        EXPECTS,      // links, but the fragment shader's `expects` refuses the vertex shader's topology
        LINK_FAILED,  // glslangValidator -l fails
    };

    std::string expectsText(const shader::SSource& frag) {
        std::string out;
        for (const auto k : frag.expects)
            out += (out.empty() ? "" : ",") + std::string{shader::topologyName(k)};
        return out.empty() ? "any" : out;
    }

    void printPairTable(const std::vector<SAssembled>& verts, const std::vector<SAssembled>& frags, const std::vector<ePair>& result) {
        std::vector<std::string> rowLabels;
        size_t                   width = 0;
        for (const auto& v : verts) {
            rowLabels.push_back(std::format("{} ({})", v.label, shader::topologyText(v.src.topology.value_or(shader::eTopology::PATH), v.src.instances)));
            width = std::max(width, rowLabels.back().size());
        }

        std::cout << "shader pairs: ok = links and passes expects, expects = links but the loader refuses the pair (not a failure), LINK = does not link\n";
        for (size_t j = 0; j < frags.size(); ++j)
            std::cout << std::format("  F{:<2} {} (expects {})\n", j + 1, frags[j].label, expectsText(frags[j].src));
        std::cout << std::format("  {:<{}}", "", width);
        for (size_t j = 0; j < frags.size(); ++j)
            std::cout << std::format(" {:>7}", std::format("F{}", j + 1));
        std::cout << "\n";
        for (size_t i = 0; i < verts.size(); ++i) {
            std::cout << std::format("  {:<{}}", rowLabels[i], width);
            for (size_t j = 0; j < frags.size(); ++j) {
                const auto r = result[i * frags.size() + j];
                std::cout << std::format(" {:>7}", r == ePair::OK ? "ok" : r == ePair::EXPECTS ? "expects" : "LINK");
            }
            std::cout << "\n";
        }
    }
}

// Every vertex shader linked with every fragment shader, shipped
// (hyprtail/shaders) and demo (demo/hyprtail/shaders) alike, found by glob so
// a new layer joins the matrix without editing this test. Each is assembled by
// the loader's own preprocessor (the shipped ones from the embedded table as
// CShaderSlot::builtin() does, the demo ones from disk as CShaderSlot::reload()
// does), then linked by glslangValidator -l: the dialect is the shader's own
// `#version 300 es`, as for the single-file checks `make test-unit` runs
// afterwards on the files this leaves in $OUT_DIR. No skip list: a pair that
// doesn't link fails.
//
// For every pair the loader's own expectsMismatch() is also evaluated and the
// whole matrix printed. An `expects` refusal is reported in the table and is
// not a failure: it is the loader declining a pairing that links.
//
// Not covered, because it lives in ShaderSlot.cpp and needs GL: the rest of
// the loader's pair checks (param consistency, padding names) and the
// driver's own link.
static void testShaderLinkMatrix() {
    namespace fs = std::filesystem;
    if (!fs::is_directory(SHIPPED_SHADERS))
        return;

    const char*    outDir = std::getenv("OUT_DIR");
    const fs::path dir    = outDir ? fs::path{outDir} : fs::temp_directory_path() / std::format("hyprtail-glsl-{}", getpid());
    fs::create_directories(dir);

    const auto assemble = [&dir](const std::string& label, std::expected<shader::SSource, std::string> src, const std::string& stem) -> std::optional<SAssembled> {
        if (!src) {
            std::cerr << std::format("{} doesn't preprocess: {}\n", label, src.error());
            return std::nullopt;
        }
        const auto file = (dir / stem).string();
        std::ofstream(file) << src->text;
        return SAssembled{.label = label, .file = file, .src = std::move(*src)};
    };

    std::vector<SAssembled> vertShaders, fragShaders;
    for (const auto& [ext, stage, out] : {std::tuple{".vert", shader::eStage::VERTEX, &vertShaders}, std::tuple{".frag", shader::eStage::FRAGMENT, &fragShaders}}) {
        for (const auto& file : globShaders(SHIPPED_SHADERS, ext)) {
            const auto key  = "shaders/" + file;
            const auto text = shader::builtin(key);
            if (text.empty()) {
                std::cerr << std::format("hyprtail/{} exists but isn't in the embedded table (shader::builtin(), src/ShaderSource.cpp)\n", key);
                ++s_failed;
                continue;
            }
            if (auto a = assemble(file, shader::preprocess(text, key, {}, stage), file))
                out->push_back(std::move(*a));
            else
                ++s_failed;
        }
        for (const auto& file : globShaders(DEMO_SHADERS, ext)) {
            if (auto a = assemble("demo/" + file, shader::load(fs::path{DEMO_SHADERS} / file, stage), "demo_" + file))
                out->push_back(std::move(*a));
            else
                ++s_failed;
        }
    }
    CHECK(!vertShaders.empty() && !fragShaders.empty());

    if (runCapture({"glslangValidator", "--version"}).first == 127) {
        std::cerr << "glslangValidator not found, skipping the GLSL link matrix\n";
        if (!outDir)
            fs::remove_all(dir);
        return;
    }

    // Control, reported and not asserted: a fragment input the vertex stage
    // doesn't declare is the failure class the driver's link rejects (see
    // varyingCheck, ShaderSlot.cpp). If glslangValidator -l accepts it, this
    // matrix can't see that class, and says so.
    {
        const auto cv = assemble("canary.vert", pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology quad\nvoid main() { ht_initVaryings(); gl_Position = ht_toClip(ht_anchor); }\n",
                                                   shader::eStage::VERTEX),
                                 "canary.vert");
        const auto cf = assemble("canary.frag", pp("#version 300 es\n#pragma hyprtail contract 2\nin float ht_notWritten;\nvoid main() { ht_fragColor = vec4(ht_notWritten); }\n", shader::eStage::FRAGMENT),
                                 "canary.frag");
        CHECK(cv && cf);
        if (cv && cf) {
            const auto [status, log] = runCapture({"glslangValidator", "-l", cv->file, cf->file});
            if (status == 0)
                std::cerr << "note: glslangValidator -l links a fragment input the vertex shader never declares (control pair canary.vert + canary.frag): this matrix "
                             "cannot detect a fragment shader reading a varying that a vertex shader lacks\n";
            fs::remove(cv->file);
            fs::remove(cf->file);
        }
    }

    std::vector<ePair> result;
    size_t             refused = 0, failed = 0;
    for (const auto& v : vertShaders) {
        for (const auto& f : fragShaders) {
            const auto [status, log] = runCapture({"glslangValidator", "-l", v.file, f.file});
            if (status != 0) {
                ++s_failed;
                ++failed;
                result.push_back(ePair::LINK_FAILED);
                std::cerr << std::format("GLSL link FAILED: {} + {}\n{}", v.label, f.label, readableLog(log, v, f));
                continue;
            }
            ++s_passed;
            if (shader::expectsMismatch(v.src, f.src)) {
                ++refused;
                result.push_back(ePair::EXPECTS);
            } else
                result.push_back(ePair::OK);
        }
    }

    printPairTable(vertShaders, fragShaders, result);
    const size_t pairs = vertShaders.size() * fragShaders.size();
    std::cout << std::format("{} vertex x {} fragment = {} pairs: {} ok, {} refused by expects, {} failed to link\n", vertShaders.size(), fragShaders.size(), pairs, pairs - refused - failed,
                             refused, failed);

    if (!outDir)
        fs::remove_all(dir);
}

// The demo presets (demo/hyprtail/presets, not embedded and not shipped): each
// has a header comment, names layers only from its own `layers`, resolves
// every shader the way a file preset does (prefab:<name> is the embedded
// shaders/<name>, anything else a path under demo/hyprtail), pairs stages
// the loader accepts (expectsMismatch), and sets only parameters that its
// program declares, with values of the right type inside the declared range.
// The same rules as the shipped manifests in testPresetManifests, without its
// embedded-copy comparison.
static void testDemoPresets() {
    namespace fs = std::filesystem;
    const fs::path root = "demo/hyprtail";
    if (!fs::is_directory(root / "presets"))
        return;

    const auto trim = [](std::string s) {
        const auto ws = " \t\r";
        s.erase(0, s.find_first_not_of(ws));
        s.erase(s.find_last_not_of(ws) + 1);
        return s;
    };
    const auto resolve = [&root](const std::string& value, shader::eStage stage) -> std::expected<shader::SSource, std::string> {
        if (value.starts_with("prefab:")) {
            const auto key = "shaders/" + value.substr(7);
            if (shader::builtin(key).empty())
                return std::unexpected(std::format("{} isn't a built-in shader", value));
            return shader::preprocess(shader::builtin(key), key, {}, stage);
        }
        return shader::load(root / value, stage);
    };

    std::set<std::string> allLayers; // layer names are unique across the demo presets
    int                   manifests = 0;
    for (const auto& entry : fs::directory_iterator(root / "presets")) {
        if (entry.path().extension() != ".conf")
            continue;
        ++manifests;
        const auto name = entry.path().filename().string();
        const auto fail = [&](const std::string& why) {
            std::cerr << std::format("demo preset {}: {}\n", name, why);
            CHECK(false);
        };

        std::map<std::string, std::map<std::string, std::string>> layers; // layer -> key -> value
        std::string                                               contract, layerList, kind = std::string{source::DEFAULT_KIND};
        std::ifstream                                             in(entry.path());
        std::string                                               line;
        bool                                                      header = false, first = true;
        while (std::getline(in, line)) {
            if (first && !trim(line).empty()) {
                header = trim(line).starts_with('#');
                first  = false;
            }
            line          = trim(line.substr(0, line.find('#')));
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (const auto colon = key.find(':'); colon != std::string::npos)
                layers[trim(key.substr(0, colon))][trim(key.substr(colon + 1))] = value;
            else if (key == "contract")
                contract = value;
            else if (key == "layers")
                layerList = value;
            else if (key == "source")
                kind = value;
            else if (key != "description")
                fail(std::format("unknown key \"{}\"", key));
        }
        if (!header)
            fail("must start with a header comment");
        if (contract != std::to_string(shader::CONTRACT_VERSION))
            fail("missing or wrong contract");
        if (!source::known(kind)) {
            fail(std::format("unknown source \"{}\"", kind));
            continue;
        }

        std::vector<std::string> names;
        std::stringstream        ls(layerList);
        for (std::string l; std::getline(ls, l, ',');)
            names.push_back(trim(l));
        if (names.empty() || names.size() > 4)
            fail("needs 1 to 4 layers");

        for (const auto& [layer, keys] : layers) {
            if (layer == source::KEY_PREFIX) {
                const auto& decls = source::decls(kind);
                for (const auto& [key, text] : keys) {
                    const auto decl = std::ranges::find_if(decls, [&](const auto& d) { return d.name == key; });
                    if (decl == decls.end()) {
                        fail(std::format("\"{}\" isn't a setting of source {}", key, kind));
                        continue;
                    }
                    auto v = params::parseValue(decl->type, text);
                    if (v)
                        if (auto r = params::checkRange(*decl, *v); !r)
                            v = std::unexpected(r.error());
                    if (!v)
                        fail(std::format("source:{} = {}: {}", key, text, v.error()));
                }
            } else if (std::ranges::find(names, layer) == names.end())
                fail(std::format("\"{}:...\" keys given, but \"{}\" isn't in layers", layer, layer));
        }

        for (const auto& layer : names) {
            if (!allLayers.insert(layer).second)
                fail(std::format("layer name \"{}\" is also used by another demo preset", layer));
            const auto it = layers.find(layer);
            if (it == layers.end() || !it->second.contains("vertex") || !it->second.contains("fragment")) {
                fail(std::format("layer {} needs vertex and fragment", layer));
                continue;
            }
            const auto& keys = it->second;
            const auto  vert = resolve(keys.at("vertex"), shader::eStage::VERTEX);
            const auto  frag = resolve(keys.at("fragment"), shader::eStage::FRAGMENT);
            if (!vert || !frag) {
                fail(std::format("layer {}: {}", layer, !vert ? vert.error() : frag.error()));
                continue;
            }
            if (const auto mismatch = shader::expectsMismatch(*vert, *frag))
                fail(std::format("layer {}: {}", layer, *mismatch));

            std::vector<params::SDecl> decls;
            for (const auto& r : shader::reservedParams())
                decls.push_back(r.decl);
            for (const auto* src : {&*vert, &*frag})
                for (const auto& p : src->params)
                    decls.push_back(p.decl);

            for (const auto& [key, text] : keys) {
                if (key == "vertex" || key == "fragment")
                    continue;
                const auto decl = std::ranges::find_if(decls, [&](const auto& d) { return d.name == key; });
                if (decl == decls.end()) {
                    fail(std::format("layer {}: \"{}\" isn't a parameter of {} + {}", layer, key, keys.at("vertex"), keys.at("fragment")));
                    continue;
                }
                auto v = params::parseValue(decl->type, text);
                if (v)
                    if (auto r = params::checkRange(*decl, *v); !r)
                        v = std::unexpected(r.error());
                if (!v)
                    fail(std::format("layer {}: {} = {}: {}", layer, key, text, v.error()));
            }
        }
    }
    CHECK(manifests >= 1);
}

// A test that throws aborts the run, which is the failure signal.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    testEmitConfig();
    testSpringChain();
    testSource();
    testParams();
    testShaderSource();
    testPresetManifests();
    testDemoPresets();
    testCrashGuard();
    testPointerGate();
    testRing();

    // Also leaves the assembled built-ins in $OUT_DIR for the single-file
    // validator pass in the Makefile.
    testShaderLinkMatrix();

    std::cout << std::format("unit: {} passed, {} failed\n", s_passed, s_failed);
    return s_failed ? 1 : 0;
}
