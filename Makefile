PLUGIN_NAME := hyprtail

SOURCE_FILES := $(wildcard src/*.cpp)
HEADER_FILES := $(wildcard src/*.hpp)
# Embedded into the plugin with #embed (src/ShaderSource.cpp).
SHADER_FILES := $(wildcard shaders/*.vert shaders/*.frag shaders/*/*.glsl)
# Embedded built-in preset manifests, #embed (src/Preset.cpp).
PRESET_FILES := $(wildcard presets/*.conf)

# Which Hyprland checkout DEV=1 builds against (default: the pin).
#   make DEV=1 HYPRLAND_DIR=external/Hyprland-main
# Build products go to OUT, out/<checkout name> for any checkout but the pin,
# so builds against different checkouts coexist. Only `make clean` removes
# them all.
DEV ?= 0

PINNED_DIR := $(abspath external/Hyprland)
ifneq ($(origin HYPRLAND_DIR),undefined)
    ifneq ($(DEV),1)
        $(error HYPRLAND_DIR only applies to DEV=1 builds; the default build takes its headers from pkg-config)
    endif
endif
HYPRLAND_DIR ?= external/Hyprland
HYPRLAND_SRC := $(abspath $(HYPRLAND_DIR))
ifeq ($(HYPRLAND_SRC),$(PINNED_DIR))
    OUT ?= out
else
    OUT ?= out/$(notdir $(HYPRLAND_SRC))
endif

OBJECT_FILES := $(patsubst src/%.cpp, $(OUT)/%.o, $(SOURCE_FILES))

OUTPUT := $(OUT)/$(PLUGIN_NAME).so

# Two ways to get Hyprland's headers:
#
#   make          Default, used by hyprpm and users. Headers from pkg-config.
#                 Under hyprpm, PKG_CONFIG_PATH points at the headers hyprpm
#                 built for the running Hyprland; otherwise the installed
#                 package's (e.g. /usr/include/hyprland).
#   make DEV=1    Development. Headers from the external/Hyprland checkout,
#                 which must be at the pinned commit and built (SPEC §2), or
#                 from another built checkout given as HYPRLAND_DIR (above).

# Pinned Hyprland commit (SPEC §2). Tracks the host package: v0.56.2.
HYPRLAND_PIN   := efb50993780079460b0cbed1363e2166a2de1d9f
HOST_VERSION_H := /usr/include/hyprland/src/version.h

ifeq ($(DEV),1)
    HEADER_CHECK    := check-pin
    # pkg-config brings in the pixman/drm/cairo/freetype includes that
    # hyprland.pc would otherwise provide.
    HYPRLAND_CFLAGS := -I$(HYPRLAND_SRC)/src -I$(HYPRLAND_SRC)/protocols $(shell pkg-config --cflags pixman-1 libdrm cairo freetype2)
else
    HEADER_CHECK    := check-headers
    HYPRLAND_CFLAGS := $(shell pkg-config --cflags hyprland 2>/dev/null)
endif

# Appended, not assigned: hyprpm passes extra CFLAGS/CXXFLAGS through the
# environment (hyprpm PluginManager.cpp getPluginBuildEnv).
CXXFLAGS += -Wall -Wno-missing-field-initializers -fPIC -std=c++26 -g $(HYPRLAND_CFLAGS)

# GCC marks some template statics STB_GNU_UNIQUE, which makes dlclose a no-op
# and breaks plugin unload/reload. Checked by compiler identity, not by name,
# so CXX=c++ still gets it.
ifneq ($(shell $(CXX) --version 2>/dev/null | grep -c "Free Software Foundation"),0)
    CXXFLAGS += --no-gnu-unique
endif

# Build revision, shown in the "loaded" notification, the log and the
# errors.log header, so it's clear which build a running Hyprland has loaded
# (hyprpm doesn't reload an already-loaded plugin after `hyprpm update`).
# Rewritten only when it changes, so it doesn't force rebuilds.
REV_HEADER := $(OUT)/rev.hpp
CXXFLAGS   += -I$(OUT)

# DEV value out/ was built with, see its rule.
BUILD_MODE := $(OUT)/build-mode

.PHONY: all clean load unload smoke test-unit test-compat check-pin check-headers check-log FORCE

all: $(OUTPUT)

$(OUTPUT): $(OBJECT_FILES)
	$(CXX) -shared $^ $(LDFLAGS) -o $@

$(OUT)/%.o: src/%.cpp $(HEADER_FILES) $(SHADER_FILES) $(PRESET_FILES) $(BUILD_MODE) | $(HEADER_CHECK) check-log
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Which headers $(OUT) was built against. Switching between `make` and
# `make DEV=1` rebuilds everything instead of linking objects built against
# the other headers. Rewritten only when it changes.
$(BUILD_MODE): FORCE
	@mkdir -p $(OUT)
	@echo "DEV=$(DEV) $(if $(filter 1,$(DEV)),DIR=$(HYPRLAND_SRC))" > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(OUT)/main.o $(OUT)/Diagnostics.o: $(REV_HEADER)

$(REV_HEADER): FORCE
	@mkdir -p $(OUT)
	@rev="$$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"; \
	if [ "$$rev" != unknown ] && ! git diff --quiet HEAD -- 2>/dev/null; then rev="$$rev-dirty"; fi; \
	printf '#pragma once\n#define HYPRTAIL_REV "%s"\n' "$$rev" > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# Default mode: fail clearly if pkg-config can't find Hyprland's headers, and
# warn (don't fail) if they're not the commit this plugin is developed
# against: it may still build and work, or not (no ABI stability across
# Hyprland commits).
check-headers:
	@if ! pkg-config --exists hyprland; then \
		echo "error: Hyprland headers not found by pkg-config (hyprland.pc)." >&2; \
		echo "       Install your distribution's Hyprland development headers, build with hyprpm, or use 'make DEV=1'." >&2; \
		exit 1; \
	fi
	@api=""; \
	for d in $$(pkg-config --cflags-only-I hyprland | sed 's/-I//g'); do \
		if [ -f "$$d/plugins/PluginAPI.hpp" ]; then api=1; break; fi; \
	done; \
	if [ -z "$$api" ]; then \
		echo "error: <plugins/PluginAPI.hpp> is not under any -I from 'pkg-config --cflags hyprland'." >&2; \
		echo "       hyprland.pc before Hyprland v0.55.0 has no -I<prefix>/hyprland/src, which hyprtail's" >&2; \
		echo "       includes need. hyprtail needs Hyprland >= v0.55.0 (developed against v0.56.2)." >&2; \
		echo "       Otherwise the development headers are incomplete: reinstall them or use 'make DEV=1'." >&2; \
		exit 1; \
	fi
	@found=""; \
	for d in $$(pkg-config --cflags-only-I hyprland | sed 's/-I//g'); do \
		if [ -f "$$d/version.h" ]; then found=$$(sed -n 's/^#define GIT_COMMIT_HASH *"\(.*\)"/\1/p' "$$d/version.h"); break; fi; \
	done; \
	if [ -n "$$found" ] && [ "$$found" != "$(HYPRLAND_PIN)" ]; then \
		echo "warning: building against Hyprland $$found; hyprtail is developed against $(HYPRLAND_PIN) (v0.56.2) and may not build or work with other versions" >&2; \
	fi

# DEV=1: the checkout must have been built (version.h and the protocol
# headers are generated by its build). external/Hyprland must also be at the
# pin, and warns if the installed host headers moved off it, e.g. after a
# package upgrade: the plugin would then fail the ABI hash check when loaded
# into the host. Any other checkout (HYPRLAND_DIR) is a compatibility build:
# it only reports which commit it built against.
check-pin:
	@head=$$(git -C $(HYPRLAND_SRC) rev-parse HEAD 2>/dev/null); \
	if [ "$(HYPRLAND_SRC)" = "$(PINNED_DIR)" ] && [ "$$head" != "$(HYPRLAND_PIN)" ]; then \
		echo "error: external/Hyprland is at '$$head', pin is $(HYPRLAND_PIN) (SPEC §2)" >&2; exit 1; \
	fi; \
	if [ "$(HYPRLAND_SRC)" != "$(PINNED_DIR)" ]; then \
		echo "note: building against $(HYPRLAND_SRC) at '$$head', not the pin $(HYPRLAND_PIN)" >&2; \
	fi
	@if [ ! -f $(HYPRLAND_SRC)/src/version.h ]; then \
		echo "error: $(HYPRLAND_SRC)/src/version.h missing, run 'make clear && make debug' in $(HYPRLAND_SRC)" >&2; exit 1; \
	fi
	@if [ "$(HYPRLAND_SRC)" = "$(PINNED_DIR)" ] && [ -f $(HOST_VERSION_H) ] && ! grep -q '"$(HYPRLAND_PIN)"' $(HOST_VERSION_H); then \
		echo "warning: installed Hyprland headers ($(HOST_VERSION_H)) are not at the pin; host package moved? re-pin before loading on the host" >&2; \
	fi

# Hyprland's own Log::logger->log() must only be called from compat.hpp: on
# main the same-looking call can compile and print the wrong thing (compat.hpp).
check-log:
	@if grep -n 'logger->log(' $(filter-out src/compat.hpp,$(SOURCE_FILES) $(HEADER_FILES)); then \
		echo "error: call hyprtail::compat::log(), not Log::logger->log() (src/compat.hpp)" >&2; exit 1; \
	fi

clean:
	$(RM) -r out/ $(OUT)

# These talk to whichever Hyprland instance $HYPRLAND_INSTANCE_SIGNATURE
# points at: run them from a terminal inside the instance you mean.
load: all
	hyprctl plugin load $(CURDIR)/$(OUTPUT)

unload:
	hyprctl plugin unload $(CURDIR)/$(OUTPUT)

# Unit tests for the Hyprland-free parts (parameter pragmas, padding
# expressions, shader preprocessing, node ring), then the preprocessed
# built-in shaders through glslangValidator (GLSL ES 3.00 syntax and
# semantics, no GPU). No compositor involved.
# `make test-unit SANITIZE=1` builds with AddressSanitizer and UBSan.
UNIT_OUT := out/unit
UNIT_FLAGS := $(if $(filter 1,$(SANITIZE)),-O0 -fsanitize=address -fsanitize=undefined,)
# The spring source calls hyprutils' advanceSpring (SpringChain.cpp): the one
# Hyprland-family library the unit tests link, for its header and symbol only.
HYPRUTILS  := $(shell pkg-config --cflags --libs hyprutils 2>/dev/null || echo -lhyprutils)

test-unit:
	@mkdir -p $(UNIT_OUT)
	$(CXX) -std=c++26 -Wall -g $(UNIT_FLAGS) tests/unit/unit.cpp src/Params.cpp src/ShaderSource.cpp src/Source.cpp src/SpringChain.cpp src/TrailBuffer.cpp src/CrashGuard.cpp $(HYPRUTILS) -o $(UNIT_OUT)/unit
	rm -rf $(UNIT_OUT)/glsl
	OUT_DIR=$(UNIT_OUT)/glsl $(UNIT_OUT)/unit
	@command -v glslangValidator >/dev/null || { echo "glslangValidator not found, skipping the GLSL check" >&2; exit 0; }; \
	status=0; for f in $(UNIT_OUT)/glsl/*; do \
		if glslangValidator "$$f" >$$f.log 2>&1; then echo "glsl ok: $$f"; else echo "glsl FAILED: $$f" >&2; cat $$f.log >&2; status=1; fi; \
	done; \
	for v in $(UNIT_OUT)/glsl/*.vert; do for fr in $(UNIT_OUT)/glsl/*.frag; do \
		if glslangValidator -l "$$v" "$$fr" >$(UNIT_OUT)/glsl/link.log 2>&1; then echo "glsl link ok: $$(basename $$v) + $$(basename $$fr)"; \
		else echo "glsl link FAILED: $$v + $$fr" >&2; cat $(UNIT_OUT)/glsl/link.log >&2; status=1; fi; \
	done; done; exit $$status

# Compile-only check of src/compat.hpp against the selected Hyprland headers
# (`make test-compat`, `make DEV=1 test-compat [HYPRLAND_DIR=...]`): every
# wrapper is instantiated and the assumptions it makes are asserted.
test-compat: | $(HEADER_CHECK)
	@mkdir -p $(OUT)/compat
	$(CXX) $(CXXFLAGS) -c tests/compat/compat.cpp -o $(OUT)/compat/compat.o

# Lifecycle smoke test (SPEC §10): load, duplicate refusal, monitor hotplug,
# unload and reload of the plugin in a headless Hyprland started from the
# external/Hyprland checkout, which must be built with tests (`make clear &&
# make debug` there). Run it at every re-pin, from a terminal in a Wayland
# session.
#
# Environment for the test Hyprland (NOTES "Smoke test environment"):
# - Its own XDG_RUNTIME_DIR, so hyprtester (which talks to the newest
#   instance under $XDG_RUNTIME_DIR/hypr, hyprtester/src/hyprctlCompat.cpp:26-80)
#   can't reach the running session. Short: the socket path is the runtime
#   dir plus 82 characters and must fit in 107 (Hyprland Compositor.cpp:192-217,
#   EventManager.cpp:21-24; HyprCtl.cpp:2331-2333 silently truncates instead).
# - The session's Wayland socket as an absolute WAYLAND_DISPLAY: at this pin
#   Hyprland always starts headless + DRM-if-available + Wayland-fallback
#   (Compositor.cpp:307-319, HYPRLAND_HEADLESS_ONLY isn't read), the headless
#   backend has no DRM fd, and without an allocator from DRM (unavailable
#   inside a session) or Wayland, aquamarine's start() fails ("CBackend::create()
#   failed!", aquamarine Backend.cpp:163-178). libwayland accepts an absolute
#   WAYLAND_DISPLAY independent of XDG_RUNTIME_DIR (wayland-client.c:1164-1185).
#   The test Hyprland is a Wayland client of the session: nothing is loaded
#   into it. Its one Wayland output is disabled by test.lua's catch-all rule.
# - Scratch XDG_STATE_HOME, so errors.log is the test's own, and scratch
#   XDG_CONFIG_HOME, where the test writes the user preset it stacks layers
#   with (hyprtail reads presets from $XDG_CONFIG_HOME/hypr/hyprtail/).
# The test file and config are copied into the checkout for the build and
# removed afterwards. On failure the scratch directory (Hyprland log under
# hypr/, errors.log under state/hyprtail/) is kept and its path printed.
HYPRTESTER_DIR := $(HYPRLAND_SRC)/hyprtester
SMOKE_TEST     := $(HYPRTESTER_DIR)/src/tests/main/hyprtail_smoke.cpp
SMOKE_CONFIG   := $(HYPRTESTER_DIR)/hyprtail_smoke.lua

smoke:
	$(MAKE) DEV=1 all
	@if [ ! -x $(HYPRLAND_SRC)/build/Hyprland ] || [ ! -d $(HYPRLAND_SRC)/build/hyprtester ]; then \
		echo "error: external/Hyprland isn't built with tests; run 'make clear && make debug' in external/Hyprland" >&2; exit 1; \
	fi
	@case "$$WAYLAND_DISPLAY" in \
		"") echo "error: run make smoke inside a Wayland session (WAYLAND_DISPLAY unset); the test Hyprland needs it for a GPU allocator" >&2; exit 1 ;; \
		/*) wl="$$WAYLAND_DISPLAY" ;; \
		*) wl="$$XDG_RUNTIME_DIR/$$WAYLAND_DISPLAY" ;; \
	esac; \
	if [ ! -S "$$wl" ]; then echo "error: session Wayland socket $$wl not found" >&2; exit 1; fi; \
	tmp=$$(mktemp -d /tmp/hts.XXXXXX) || exit 1; \
	if [ $${#tmp} -gt 25 ]; then echo "error: runtime dir $$tmp too long for Hyprland's socket paths" >&2; rm -rf "$$tmp"; exit 1; fi; \
	trap 'rm -f $(SMOKE_TEST) $(SMOKE_CONFIG)' EXIT; \
	cp tests/hyprtester/hyprtail_smoke.cpp $(SMOKE_TEST) && \
	cat $(HYPRTESTER_DIR)/test.lua tests/hyprtester/smoke.lua > $(SMOKE_CONFIG) && \
	cmake --build $(HYPRLAND_SRC)/build --target hyprtester -j$$(nproc) || exit 1; \
	mkdir -p "$$tmp/state" || exit 1; \
	cd $(HYPRLAND_SRC) && env -u DISPLAY -u HYPRLAND_INSTANCE_SIGNATURE \
		WAYLAND_DISPLAY="$$wl" XDG_RUNTIME_DIR="$$tmp" XDG_STATE_HOME="$$tmp/state" XDG_CONFIG_HOME="$$tmp/config" HYPRTAIL_SO="$(CURDIR)/$(OUTPUT)" \
		./build/hyprtester/hyprtester -c $(SMOKE_CONFIG) -b ./build/Hyprland -p hyprtester/plugin/hyprtestplugin.so hyprtailLifecycle; \
	status=$$?; \
	if [ $$status -eq 0 ]; then rm -rf "$$tmp"; echo "smoke: passed"; \
	else echo "smoke: FAILED (exit $$status); logs kept in $$tmp" >&2; fi; \
	exit $$status
