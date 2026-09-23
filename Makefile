PLUGIN_NAME := hyprtail-stage1

SOURCE_FILES := $(wildcard src/*.cpp)
HEADER_FILES := $(wildcard src/*.hpp)
OBJECT_FILES := $(patsubst src/%.cpp, out/%.o, $(SOURCE_FILES))

HYPRLAND_SRC := $(CURDIR)/external/Hyprland

# Local Hyprland headers first so they override any system-installed version.
# pkg-config brings in pixman/drm/cairo/freetype transitive includes.
CXXFLAGS := -Wall -Wno-missing-field-initializers -fPIC -std=c++26 -g \
    -I$(HYPRLAND_SRC)/src \
    -I$(HYPRLAND_SRC)/protocols \
    $(shell pkg-config --cflags pixman-1 libdrm cairo freetype2)

ifeq ($(CXX),g++)
    CXXFLAGS += --no-gnu-unique
endif

OUTPUT := out/$(PLUGIN_NAME).so

.PHONY: all clean load unload

all: $(OUTPUT)

$(OUTPUT): $(OBJECT_FILES)
	$(CXX) -shared $^ $(LDFLAGS) -o $@

out/%.o: src/%.cpp $(HEADER_FILES)
	@mkdir -p out
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) -r out/

load: all
	hyprctl plugin load $(CURDIR)/$(OUTPUT)

unload:
	hyprctl plugin unload $(CURDIR)/$(OUTPUT)
