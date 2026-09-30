// hyprtail lifecycle smoke test for Hyprland's hyprtester (SPEC §10).
//
// Not built in this repo: `make smoke` copies this file into
// external/Hyprland/hyprtester/src/tests/main/ (hyprtester globs its sources,
// hyprtester/CMakeLists.txt:14), builds hyprtester, runs only this test
// against a headless Hyprland started with smoke.lua appended to
// hyprtester/test.lua, and removes the copies again.
//
// Environment (set by `make smoke`, inherited by the Hyprland it starts: the
// hyprutils CProcess only adds variables before execvp, Process.cpp:183-234):
//   HYPRTAIL_SO      the plugin to test
//   XDG_STATE_HOME   scratch dir; the plugin writes hyprtail/errors.log there
//   XDG_CONFIG_HOME  scratch dir; user presets (hypr/hyprtail/presets/) go here
//
// Checks after every step: the compositor still answers IPC (getFromSocket
// returns "" once it can't connect, hyprctlCompat.cpp:82-103) and errors.log
// has nothing past its header line.
//
// State only, no rendering checks: the instanced-topology step (4b) loads
// each instanced preset, changes K and the capacity live, unplugs an output
// while one draws, stacks path, quad and instanced layers and switches presets
// mid-run, and asks only that the compositor lives, errors.log stays clean
// and `hyprctl hyprtail` reports the layers compiled and not disabled. The
// spring-chain step (4c) does the same for continuous upload and for switching
// the preset's source at runtime, and also asks that the chain comes to rest.

#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {
    // Output added and removed by the hotplug rounds. Its monitor rule (mode,
    // position far right of everything else) is in smoke.lua: a rule set with
    // /eval would be dropped by the config reload every plugin load triggers
    // (PluginSystem.cpp:135).
    constexpr const char* TEST_OUTPUT = "HYPRTAIL-TEST";
    constexpr int         OUT_X       = 20000;
    constexpr int         OUT_Y       = 0;

    // Matches trail:fade_ms in the plugin.hyprtail.params string in smoke.lua.
    constexpr int FADE_MS = 500;

    // Marker of our entry in `/plugin list` (HyprCtl.cpp dispatchPlugin).
    constexpr const char* LISTED = "Plugin hyprtail ";

    void sleepMs(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    std::string env(const char* name) {
        const char* v = std::getenv(name);
        return v ? v : "";
    }

    bool alive() {
        return !getFromSocket("/version").empty();
    }

    // Poll until pred holds, up to timeoutMs.
    bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
        for (int waited = 0; waited < timeoutMs; waited += 20) {
            if (pred())
                return true;
            sleepMs(20);
        }
        return pred();
    }

    // Pointer from (x0, y0) to (x1, y1) in steps about a frame apart. Goes
    // through CPointerController::warpTo, i.e. our warp hook
    // (hl.dsp.cursor.move -> Actions::moveCursor, ConfigActions.cpp:1181-1185).
    // Returns "ok" or the first failing reply.
    std::string moveAlong(int x0, int y0, int x1, int y1, int steps) {
        for (int i = 0; i <= steps; ++i) {
            const int  x = x0 + (x1 - x0) * i / steps;
            const int  y = y0 + (y1 - y0) * i / steps;
            const auto r = getFromSocket(std::format("/dispatch hl.dsp.cursor.move({{ x = {}, y = {} }})", x, y));
            if (r != "ok")
                return r;
            sleepMs(16);
        }
        return "ok";
    }

    // Plugin settings smoke.lua reads when the config is (re)loaded; see
    // plugin_settings() there. capacity 0 = unset (the plugin default).
    std::filesystem::path pluginSettingsFile() {
        return std::filesystem::path{env("XDG_STATE_HOME")} / "hyprtail-smoke-plugin.conf";
    }

    void writePluginSettings(const std::string& preset, const std::string& params, int capacity) {
        std::ofstream out(pluginSettingsFile(), std::ios::trunc);
        out << "preset=" << preset << "\nparams=" << params << "\n";
        if (capacity > 0)
            out << "capacity=" << capacity << "\n";
    }

    // hyprtail's presets directory in the scratch XDG_CONFIG_HOME
    // (cfg::hyprtailRoot() / "presets").
    std::filesystem::path userPresetsDir() {
        return std::filesystem::path{env("XDG_CONFIG_HOME")} / "hypr" / "hyprtail" / "presets";
    }

    std::string status() {
        return getFromSocket("/hyprtail");
    }

    bool statusHas(const std::string& needle) {
        return status().contains(needle);
    }

    std::filesystem::path errorsLog() {
        const auto state = env("XDG_STATE_HOME");
        return state.empty() ? std::filesystem::path{} : std::filesystem::path{state} / "hyprtail" / "errors.log";
    }

    // Everything in errors.log after its header line, or why it can't be
    // checked. Empty means clean.
    std::string errorEntries() {
        const auto path = errorsLog();
        if (path.empty())
            return "XDG_STATE_HOME is not set (run this through `make smoke`)";

        std::ifstream in(path);
        if (!in)
            return std::format("{} doesn't exist (the plugin never initialized?)", path.string());

        std::string header;
        std::getline(in, header);
        if (!header.starts_with("hyprtail "))
            return std::format("{} has an unexpected first line: {}", path.string(), header);

        std::stringstream rest;
        rest << in.rdbuf();
        const std::string entries = rest.str();
        return entries.find_first_not_of(" \t\n") == std::string::npos ? "" : entries;
    }
}

// Compositor still answering; fail the test here otherwise.
#define HYPRTAIL_ALIVE(step)                                                                                                                                                       \
    do {                                                                                                                                                                           \
        if (!alive())                                                                                                                                                              \
            FAIL_TEST("compositor died or stopped responding: {}", step);                                                                                                        \
        LOG_OK("compositor alive: {}", step);                                                                                                                                      \
    } while (0)

// Nothing reported by the plugin (errors or warnings) so far this load.
#define HYPRTAIL_NO_ERRORS(step)                                                                                                                                                   \
    do {                                                                                                                                                                           \
        if (const auto ENTRIES = errorEntries(); !ENTRIES.empty())                                                                                                                 \
            FAIL_TEST("errors.log after {}:\n{}", step, ENTRIES);                                                                                                                  \
        LOG_OK("errors.log clean: {}", step);                                                                                                                                      \
    } while (0)

// Apply new plugin settings through a config reload (smoke.lua re-reads
// them), and let the plugin settle.
#define HYPRTAIL_CONFIGURE(step, preset, params, capacity)                                                                                                                         \
    do {                                                                                                                                                                           \
        writePluginSettings(preset, params, capacity);                                                                                                                             \
        OK(getFromSocket("/reload"));                                                                                                                                              \
        sleepMs(300);                                                                                                                                                              \
        HYPRTAIL_ALIVE(step);                                                                                                                                                      \
    } while (0)

// Pointer motion so the output renders (rendering compiles the layers), then
// wait for the status to show `needle`.
#define HYPRTAIL_EXPECT_STATUS(step, needle)                                                                                                                                       \
    do {                                                                                                                                                                           \
        OK(moveAlong(700, 400, 1200, 700, 20));                                                                                                                                    \
        if (!waitFor([&] { return statusHas(needle); }, 3000))                                                                                                                     \
            FAIL_TEST("{}: `hyprctl hyprtail` never showed \"{}\":\n{}", step, needle, status());                                                                                 \
        LOG_OK("status shows {}: {}", needle, step);                                                                                                                               \
    } while (0)

// No layer or node buffer failed, and errors.log is clean.
#define HYPRTAIL_HEALTHY(step)                                                                                                                                                     \
    do {                                                                                                                                                                           \
        HYPRTAIL_ALIVE(step);                                                                                                                                                      \
        if (const auto S = status(); S.contains("DISABLED") || S.contains("NODE BUFFER FAILED"))                                                                                   \
            FAIL_TEST("a layer or the node buffer failed after {}:\n{}", step, S);                                                                                                 \
        HYPRTAIL_NO_ERRORS(step);                                                                                                                                                  \
    } while (0)

TEST_CASE(hyprtailLifecycle) {
    const auto so = env("HYPRTAIL_SO");
    if (so.empty() || !std::filesystem::is_regular_file(so))
        FAIL_TEST("HYPRTAIL_SO must name the plugin to test, got '{}'", so);

    ASSERT_NOT_CONTAINS(getFromSocket("/plugin list"), LISTED);

    // 1. Load. The reply comes once loadPlugin has finished (HyprCtl.cpp:1824-1839).
    OK(getFromSocket("/plugin load " + so));
    HYPRTAIL_ALIVE("load");
    ASSERT_COUNT_STRING(getFromSocket("/plugin list"), LISTED, 1);
    // The config reload scheduled after a load (PluginSystem.cpp:135) applies
    // smoke.lua's plugin settings.
    sleepMs(300);
    HYPRTAIL_ALIVE("config reload after load");
    HYPRTAIL_NO_ERRORS("load");

    // `hyprctl hyprtail` answers in both formats (flags before '/', as
    // hyprctl sends them).
    ASSERT_STARTS_WITH(getFromSocket("/hyprtail"), "hyprtail ");
    ASSERT_STARTS_WITH(getFromSocket("j/hyprtail"), "{\"rev\"");
    HYPRTAIL_ALIVE("status command");

    // 2. Trail, then the idle effect (delay 50 ms, duration 200 ms in
    // smoke.lua) once the pointer stops, on an existing output.
    OK(moveAlong(700, 400, 1200, 700, 20));
    sleepMs(FADE_MS + 200);
    HYPRTAIL_ALIVE("trail and idle effect");
    HYPRTAIL_NO_ERRORS("trail and idle effect");

    // 3. A second copy from another path must be refused (duplicate check in
    // PLUGIN_INIT); the first stays loaded and untouched.
    {
        const auto      dup = std::filesystem::temp_directory_path() / "hyprtail-smoke-duplicate.so";
        std::error_code ec;
        std::filesystem::copy_file(so, dup, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            FAIL_TEST("can't copy the plugin to {}: {}", dup.string(), ec.message());

        const auto reply = getFromSocket("/plugin load " + dup.string());
        std::filesystem::remove(dup, ec);

        NOK(reply);
        HYPRTAIL_ALIVE("refused duplicate load");
        ASSERT_COUNT_STRING(getFromSocket("/plugin list"), LISTED, 1);
        HYPRTAIL_NO_ERRORS("refused duplicate load");
    }

    // 4. Hotplug. Several rounds, so a new CMonitor may land at a freed one's
    // address. Each round draws a trail on the new output and removes it
    // while that trail is still fading.
    for (int round = 1; round <= 3; ++round) {
        const auto step = std::format("hotplug round {}", round);

        OK(getFromSocket(std::format("/output create headless {}", TEST_OUTPUT)));
        HYPRTAIL_ALIVE(step + ": output added");
        // The monitor rule is applied on a deferred reload of the rules.
        if (!waitFor([] { return getFromSocket("/monitors").contains(std::format("at {}x{}", OUT_X, OUT_Y)); }, 2000))
            FAIL_TEST("{}: {} never got its smoke.lua position {}x{}:\n{}", step, TEST_OUTPUT, OUT_X, OUT_Y, getFromSocket("/monitors"));

        OK(moveAlong(OUT_X + 100, OUT_Y + 100, OUT_X + 1000, OUT_Y + 600, 15));
        OK(getFromSocket(std::format("/output remove {}", TEST_OUTPUT)));
        HYPRTAIL_ALIVE(step + ": output removed mid-fade");
        if (!waitFor([] { return !getFromSocket("/monitors").contains(TEST_OUTPUT); }, 2000))
            FAIL_TEST("{}: {} still listed after removal", step, TEST_OUTPUT);

        // Back on a remaining output, then let everything fade.
        OK(moveAlong(800, 400, 1100, 600, 10));
        sleepMs(FADE_MS + 100);
        HYPRTAIL_ALIVE(step + ": after fade");
        HYPRTAIL_NO_ERRORS(step);
    }

    // 4b. Instanced topology (SPEC §13.3, phase 5): new GL resource handling
    // (a second VAO on the node VBO with a divisor of K, integer attributes
    // re-pointed at the first visible node before every draw, up to 64 x 4096
    // instances in one draw). State only, see the header comment.
    {
        // Every step below keeps trail:fade_ms at FADE_MS so the waits hold.
        const std::string fade = std::format("trail:fade_ms={}", FADE_MS);

        // Each instanced preset loads, compiles and draws.
        for (const std::string preset : {"prefab:jitter", "prefab:spray"}) {
            const auto step = "instanced preset " + preset;
            HYPRTAIL_CONFIGURE(step, preset, fade, 0);
            // The status names the K param (jitter: copies, spray: count), so
            // it only matches once this preset's own program is active.
            HYPRTAIL_EXPECT_STATUS(step, preset == "prefab:jitter" ? "topology instanced copies" : "topology instanced count");
            sleepMs(FADE_MS + 200);
            HYPRTAIL_HEALTHY(step);
        }

        // K changes live through `params` on a config reload, across its
        // whole range, for a K param named in the pragma (jitter: copies,
        // spray: count). The status lists the layer's resolved values.
        const std::pair<std::string, std::string> kParams[] = {{"prefab:jitter", "copies"}, {"prefab:spray", "count"}};
        for (const auto& [preset, param] : kParams) {
            for (const int k : {32, 64, 1, 8}) {
                const auto step  = std::format("{} {}={}", preset, param, k);
                const auto value = std::format("{}={}", param, k);
                HYPRTAIL_CONFIGURE(step, preset, std::format("{} trail:{}", fade, value), 0);
                HYPRTAIL_EXPECT_STATUS(step, value);
                HYPRTAIL_HEALTHY(step);
            }
        }

        // Capacity resizes (the node buffer is recreated) while an
        // instanced layer is showing, at the largest K, up to the largest
        // capacity: 4096 nodes x 64 copies is the biggest draw there is.
        // Each reload lands in the middle of a fading trail.
        for (const int capacity : {512, 8, 4096, 2, 64}) {
            const auto step = std::format("capacity {} under an instanced layer", capacity);
            OK(moveAlong(700, 400, 1200, 700, 20));
            HYPRTAIL_CONFIGURE(step, "prefab:jitter", fade + " trail:copies=64", capacity);
            HYPRTAIL_EXPECT_STATUS(step, "topology instanced copies");
            HYPRTAIL_HEALTHY(step);
        }

        // An output unplugged while an instanced layer draws on it.
        HYPRTAIL_CONFIGURE("spray before the hotplug rounds", "prefab:spray", fade + " trail:count=16", 0);
        for (int round = 1; round <= 2; ++round) {
            const auto step = std::format("instanced hotplug round {}", round);

            OK(getFromSocket(std::format("/output create headless {}", TEST_OUTPUT)));
            HYPRTAIL_ALIVE(step + ": output added");
            if (!waitFor([] { return getFromSocket("/monitors").contains(std::format("at {}x{}", OUT_X, OUT_Y)); }, 2000))
                FAIL_TEST("{}: {} never got its smoke.lua position {}x{}:\n{}", step, TEST_OUTPUT, OUT_X, OUT_Y, getFromSocket("/monitors"));

            OK(moveAlong(OUT_X + 100, OUT_Y + 100, OUT_X + 1000, OUT_Y + 600, 15));
            OK(getFromSocket(std::format("/output remove {}", TEST_OUTPUT)));
            HYPRTAIL_ALIVE(step + ": output removed mid-fade");
            if (!waitFor([] { return !getFromSocket("/monitors").contains(TEST_OUTPUT); }, 2000))
                FAIL_TEST("{}: {} still listed after removal", step, TEST_OUTPUT);

            OK(moveAlong(800, 400, 1100, 600, 10));
            sleepMs(FADE_MS + 100);
            HYPRTAIL_HEALTHY(step);
        }

        // Path, quad and instanced layers in one preset (a user preset in the
        // scratch config directory): the shared node VAO, the quad VAO and
        // the instanced VAO in one render.
        {
            if (env("XDG_CONFIG_HOME").empty())
                FAIL_TEST("{}", "XDG_CONFIG_HOME is not set (run this through `make smoke`)");
            std::error_code ec;
            std::filesystem::create_directories(userPresetsDir(), ec);
            if (ec)
                FAIL_TEST("can't create {}: {}", userPresetsDir().string(), ec.message());
            std::ofstream(userPresetsDir() / "smoke-stack.conf") << "contract = 2\n"
                                                                    "description = smoke test: path, quad and instanced layers\n"
                                                                    "layers = trail, idle, sparks\n"
                                                                    "trail:vertex = prefab:ribbon.vert\n"
                                                                    "trail:fragment = prefab:ribbon.frag\n"
                                                                    "idle:vertex = prefab:ring.vert\n"
                                                                    "idle:fragment = prefab:ring.frag\n"
                                                                    "idle:enabled = true\n"
                                                                    "idle:start_ms = 50\n"
                                                                    "idle:duration_ms = 200\n"
                                                                    "sparks:vertex = prefab:spray.vert\n"
                                                                    "sparks:fragment = prefab:dots.frag\n"
                                                                    "sparks:count = 12\n";
        }
        HYPRTAIL_CONFIGURE("stacked preset", "smoke-stack", fade + " sparks:fade_ms=" + std::to_string(FADE_MS), 0);
        HYPRTAIL_EXPECT_STATUS("stacked preset", "layer sparks");
        if (!statusHas("topology path") || !statusHas("topology quad") || !statusHas("topology instanced count"))
            FAIL_TEST("stacked preset: expected a path, a quad and an instanced layer:\n{}", status());
        sleepMs(400); // through the idle window too
        HYPRTAIL_HEALTHY("stacked preset");

        // Presets switched while a trail is still on screen, in an order
        // that swaps instanced <-> path <-> quad state under the same VBO.
        for (const std::string preset : {"prefab:jitter", "prefab:classic", "prefab:spray", "smoke-stack", "prefab:subtle", "prefab:jitter"}) {
            const auto step = "switch to " + preset;
            OK(moveAlong(700, 400, 1200, 700, 12));
            HYPRTAIL_CONFIGURE(step, preset, fade, 0);
            OK(moveAlong(1200, 700, 800, 500, 12));
            HYPRTAIL_HEALTHY(step);
        }

        // Back to smoke.lua's own settings for the steps below.
        std::filesystem::remove(pluginSettingsFile());
        OK(getFromSocket("/reload"));
        sleepMs(300);
        OK(moveAlong(700, 400, 1200, 700, 20));
        sleepMs(FADE_MS + 200);
        HYPRTAIL_HEALTHY("instanced topology steps");
    }

    // 4c. Spring-chain source (SPEC §13.1, §13.7): continuous upload runs
    // for real (the node buffer is rewritten every frame the chain moves),
    // and a preset's source can change at runtime. State only, see the
    // header comment.
    {
        const std::string fade = std::format("trail:fade_ms={}", FADE_MS);

        // Loads, draws, moves, and once the pointer is still the chain comes
        // to rest: the status stops reporting its points as moving. (That
        // needs renders to keep coming while it moves: the tick only runs
        // inside one.)
        HYPRTAIL_CONFIGURE("spring preset", "prefab:spring", fade, 0);
        HYPRTAIL_EXPECT_STATUS("spring preset", "source: spring");
        if (!waitFor([] { return statusHas("(moving)"); }, 1000))
            LOG_OK("{}", "spring preset: already at rest before the settle check");
        if (!waitFor([] { return !statusHas("(moving)"); }, 4000))
            FAIL_TEST("spring preset: the chain never came to rest:\n{}", status());
        sleepMs(FADE_MS + 200);
        HYPRTAIL_HEALTHY("spring preset at rest");

        // Path, instanced and quad layers over the one spring source (a
        // user preset in the scratch config directory).
        {
            std::error_code ec;
            std::filesystem::create_directories(userPresetsDir(), ec);
            if (ec)
                FAIL_TEST("can't create {}: {}", userPresetsDir().string(), ec.message());
            std::ofstream(userPresetsDir() / "smoke-spring-stack.conf") << "contract = 2\n"
                                                                           "description = smoke test: path, quad and instanced layers over a spring chain\n"
                                                                           "layers = trail, idle, sparks\n"
                                                                           "source = spring\n"
                                                                           "source:damping = 150\n"
                                                                           "trail:vertex = prefab:ribbon.vert\n"
                                                                           "trail:fragment = prefab:ribbon.frag\n"
                                                                           "idle:vertex = prefab:ring.vert\n"
                                                                           "idle:fragment = prefab:ring.frag\n"
                                                                           "idle:enabled = true\n"
                                                                           "idle:start_ms = 50\n"
                                                                           "idle:duration_ms = 200\n"
                                                                           "sparks:vertex = prefab:spray.vert\n"
                                                                           "sparks:fragment = prefab:dots.frag\n"
                                                                           "sparks:count = 12\n";
        }
        HYPRTAIL_CONFIGURE("spring stack", "smoke-spring-stack", fade + " sparks:fade_ms=" + std::to_string(FADE_MS), 0);
        HYPRTAIL_EXPECT_STATUS("spring stack", "layer sparks");
        if (!statusHas("source: spring") || !statusHas("topology path") || !statusHas("topology quad") || !statusHas("topology instanced count"))
            FAIL_TEST("spring stack: expected a spring source under a path, a quad and an instanced layer:\n{}", status());
        sleepMs(400);
        HYPRTAIL_HEALTHY("spring stack");

        // Presets switched while the chain is moving, in and out of the
        // spring source (a new source replaces the old one, the same kind
        // keeps it).
        for (const std::string preset : {"prefab:subtle", "prefab:spring", "prefab:jitter", "smoke-spring-stack", "prefab:spring", "prefab:spring"}) {
            const auto step = "switch to " + preset;
            OK(moveAlong(700, 400, 1200, 700, 12));
            HYPRTAIL_CONFIGURE(step, preset, fade, 0);
            HYPRTAIL_EXPECT_STATUS(step, preset.contains("spring") ? "source: spring" : "source: pointer");
            OK(moveAlong(1200, 700, 800, 500, 12));
            HYPRTAIL_HEALTHY(step);
        }

        // Capacity (the chain's length, the node buffer is recreated)
        // changes while the chain is moving, both ways.
        for (const int capacity : {512, 8, 4096, 2, 64}) {
            const auto step = std::format("capacity {} under a spring chain", capacity);
            OK(moveAlong(700, 400, 1200, 700, 20));
            HYPRTAIL_CONFIGURE(step, "prefab:spring", fade, capacity);
            HYPRTAIL_EXPECT_STATUS(step, std::format("/{} points", capacity));
            HYPRTAIL_HEALTHY(step);
        }

        // The source's settings change live through `params`; a bad one is
        // a warning, which HYPRTAIL_HEALTHY would catch.
        HYPRTAIL_CONFIGURE("spring params", "prefab:spring", fade + " source:damping=120 source:age_step_ms=6 source:stiffness=20000 source:mass=2", 0);
        HYPRTAIL_EXPECT_STATUS("spring params", "source: spring");
        HYPRTAIL_HEALTHY("spring params");

        // Back to smoke.lua's own settings for the steps below.
        std::filesystem::remove(pluginSettingsFile());
        OK(getFromSocket("/reload"));
        sleepMs(300);
        OK(moveAlong(700, 400, 1200, 700, 20));
        sleepMs(FADE_MS + 200);
        HYPRTAIL_HEALTHY("spring chain steps");
    }

    // 5. Unload (synchronous, HyprCtl.cpp:1840-1849), then pointer motion
    // with the hooks gone.
    OK(getFromSocket("/plugin unload " + so));
    HYPRTAIL_ALIVE("unload");
    ASSERT_NOT_CONTAINS(getFromSocket("/plugin list"), LISTED);
    // The status command went with the plugin.
    EXPECT(getFromSocket("/hyprtail"), std::string{"unknown request"});
    OK(moveAlong(700, 400, 900, 500, 5));
    HYPRTAIL_ALIVE("pointer motion after unload");

    // 6. Load again: the previous load's errors.log is kept as errors.log.1.
    OK(getFromSocket("/plugin load " + so));
    HYPRTAIL_ALIVE("reload");
    ASSERT_COUNT_STRING(getFromSocket("/plugin list"), LISTED, 1);
    sleepMs(300);
    EXPECT(std::filesystem::exists(errorsLog().string() + ".1"), true);
    OK(moveAlong(700, 400, 1200, 700, 20));
    sleepMs(FADE_MS + 200);
    HYPRTAIL_ALIVE("trail after reload");
    HYPRTAIL_NO_ERRORS("reload");

    // 7. Crash-loop guard (SPEC §2): a marker for this exact build+Hyprland
    // pair, naming a dead pid, refuses the next load; deleting the marker
    // lets it through again. The plugin is still loaded from step 6, so its
    // own marker already has the right rev/hyprland lines -- read those back
    // instead of deriving the expected values ourselves, and only tamper
    // with the pid.
    {
        const auto markerPath = std::filesystem::path{env("XDG_STATE_HOME")} / "hyprtail" / "crash-guard.marker";
        std::string realMarker;
        {
            std::ifstream in(markerPath);
            if (!in)
                FAIL_TEST("no crash-guard marker at {} while hyprtail is loaded", markerPath.string());
            std::stringstream ss;
            ss << in.rdbuf();
            realMarker = ss.str();
        }

        // Unload cleanly: teardown() removes the marker, same as a normal
        // unload. We replant it by hand right after.
        OK(getFromSocket("/plugin unload " + so));
        HYPRTAIL_ALIVE("unload before the crash-guard scenario");
        ASSERT_NOT_CONTAINS(getFromSocket("/plugin list"), LISTED);

        // A pid guaranteed dead: fork, exit immediately, reap it.
        const pid_t deadChild = fork();
        if (deadChild == 0)
            _exit(0);
        if (deadChild < 0)
            FAIL_TEST("fork() failed setting up the crash-guard scenario");
        int status = 0;
        waitpid(deadChild, &status, 0);

        std::istringstream inLines(realMarker);
        std::ostringstream tampered;
        std::string        line;
        while (std::getline(inLines, line)) {
            if (line.starts_with("pid="))
                tampered << "pid=" << deadChild << "\n";
            else
                tampered << line << "\n";
        }
        {
            std::ofstream out(markerPath, std::ios::trunc);
            out << tampered.str();
        }

        // Load must be refused: compositor stays alive, hyprtail never
        // appears in the plugin list.
        const auto refused = getFromSocket("/plugin load " + so);
        NOK(refused);
        HYPRTAIL_ALIVE("crash-guard refusal");
        ASSERT_NOT_CONTAINS(getFromSocket("/plugin list"), LISTED);

        // Deleting the marker lets the next load through.
        std::filesystem::remove(markerPath);
        OK(getFromSocket("/plugin load " + so));
        HYPRTAIL_ALIVE("load after deleting the crash-guard marker");
        ASSERT_COUNT_STRING(getFromSocket("/plugin list"), LISTED, 1);
        sleepMs(300);
        HYPRTAIL_NO_ERRORS("load after deleting the crash-guard marker");
    }

    // Leave Hyprland as we found it.
    OK(getFromSocket("/plugin unload " + so));
    HYPRTAIL_ALIVE("final unload");
    ASSERT_NOT_CONTAINS(getFromSocket("/plugin list"), LISTED);
}
