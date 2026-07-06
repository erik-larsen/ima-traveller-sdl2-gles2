# Makefile -- build "IMA Traveller" (reverse-engineered traveller 2.0c,
# Driessens & Verstappen 1996) for native (Mac/Linux/Win via ANGLE) and the
# browser (Emscripten), on the freeglut SDL2/GLES2/gl4es port.
#
# Headers and prebuilt libs are vendored under deps/ (see deps/README.md for
# provenance and ./deps/refresh.sh to re-vendor), so this repo builds
# standalone.  External requirements: SDL2 for native builds (sdl2-config on
# PATH, e.g. `brew install sdl2`) and emcc for web builds (SDL2 comes from
# Emscripten's port).
#
#   make            native build  -> ./bin-<os>-<arch>/traveller
#   make run        build + run native
#   make browser    Emscripten    -> ./web/traveller.js  (loaded by ./web/traveller.html)
#   make selftest   headless CA core check (no GL)
#   make clean
#
# Every dep path is ?= so an external tree can be swapped in, e.g.
#   make GL4ES_LIB=../gl4es/lib/libGL-native.a GL4ES_INC=../gl4es/include

APPNAME = traveller
SRC     = traveller.c ima_core.c

# ---- platform detection ----
OS := $(shell uname -s)
HW := $(shell uname -m)
ifeq ($(OS),Darwin)
	OS = mac
	DYL_EXT = dylib
else ifeq ($(OS),Linux)
	OS = linux
	DYL_EXT = so
else ifneq ($(findstring MINGW64_NT,$(OS)),)
	OS = win
	DYL_EXT = dll
	CONSOLE_FLAGS = -mconsole
else ifneq ($(findstring MSYS_NT,$(OS)),)
	OS = win
	DYL_EXT = dll
	CONSOLE_FLAGS = -mconsole
endif

BIN_DIR = ./bin-$(OS)-$(HW)
WEB_DIR = ./web
APP     = $(BIN_DIR)/$(APPNAME)
EM_OUT   = $(WEB_DIR)/$(APPNAME).js
WEB_PAGE ?= $(WEB_DIR)/$(APPNAME).html

# ---- vendored deps (all overridable; see deps/README.md) ----
DEPS ?= ./deps
FREEGLUT_INC    ?= $(DEPS)/freeglut/include
FREEGLUT_LIB    ?= $(DEPS)/freeglut/lib/native/libglut.a
EM_FREEGLUT_LIB ?= $(DEPS)/freeglut/lib/web/libglut.a
GL4ES_INC       ?= $(DEPS)/gl4es/include
GL4ES_LIB       ?= $(DEPS)/gl4es/lib/libGL-native.a
EM_GL4ES_LIB    ?= $(DEPS)/gl4es/lib/libGL-web.a
# ANGLE GLES2/EGL dynamic libs (mac-arm64 vendored; on Linux point this at
# the system Mesa libs, on Windows at ANGLE DLLs)
GLES_LIBS_PATH  ?= $(DEPS)/angle/lib-$(OS)-$(HW)

# ---- SDL2 (system; web uses Emscripten's port) ----
SDL_INC  = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

# ---- ANGLE GLES2 / EGL (static gl4es needs these supplied by the exe) ----
GLES_LIBS   = -L$(GLES_LIBS_PATH) -lGLESv2 -lEGL
GLES_DYLIBS = libGLESv2.$(DYL_EXT) libEGL.$(DYL_EXT)

ifeq ($(OS),mac)
	GLES_LINK = -Wl,-rpath,$(GLES_LIBS_PATH)
define GLES_INSTALL
	for dylib in $(GLES_DYLIBS); \
		do install_name_tool -change ./$$dylib @rpath/$$dylib $@ 2>/dev/null || true; \
	done;
endef
else ifeq ($(OS),linux)
	GLES_LINK = -Wl,-rpath,$(abspath $(GLES_LIBS_PATH))
else ifeq ($(OS),win)
define GLES_INSTALL
	for dylib in $(GLES_DYLIBS); \
		do cp $(GLES_LIBS_PATH)/$$dylib $(BIN_DIR)/$$dylib; \
	done;
endef
endif

# gl4es include must precede SDL/system includes.
# -DFREEGLUT_SDL2_GL4ES routes <GL/freeglut.h> to gl4es' GL headers.
# -DFREEGLUT_STATIC: we link freeglut's static lib.
INCS = -I$(GL4ES_INC) -I$(FREEGLUT_INC) $(SDL_INC) -DFREEGLUT_SDL2_GL4ES -DFREEGLUT_STATIC

CC   = cc
EMCC = emcc -s WASM=1

OPT_ZERO = -O0 -g
OPT_TWO = -DNDEBUG -O2
OPT = $(OPT_TWO)
EM_OPT = -DNDEBUG -O2

WARN = -Wall -Wextra -Wno-unused-parameter

EM_SDL_FLAGS    = -s USE_SDL=2 -s FULL_ES2=1
EM_OUTPUT_FLAGS = -s ALLOW_MEMORY_GROWTH=1 -s EXIT_RUNTIME=0
EM_INCS = -I$(GL4ES_INC) -I$(FREEGLUT_INC) -DFREEGLUT_SDL2_GL4ES -DFREEGLUT_STATIC

all: native

native: $(APP)

$(BIN_DIR) $(WEB_DIR):
	mkdir -p $@

# ---- native link ----
# Order: app, freeglut, gl4es (static), then ANGLE GLES/EGL, SDL, libm.
$(APP): $(SRC) ima_core.h | $(BIN_DIR)
	@test -f "$(FREEGLUT_LIB)" || { echo "ERROR: $(FREEGLUT_LIB) not found -- run ./deps/refresh.sh or set FREEGLUT_LIB."; exit 1; }
	@test -f "$(GL4ES_LIB)" || { echo "ERROR: $(GL4ES_LIB) not found -- run ./deps/refresh.sh or set GL4ES_LIB."; exit 1; }
	$(CC) $(OPT) $(WARN) $(INCS) $(SRC) \
		$(FREEGLUT_LIB) $(GL4ES_LIB) \
		$(GLES_LIBS) $(GLES_LINK) $(SDL_LIBS) -lm $(CONSOLE_FLAGS) -o $@
	$(call GLES_INSTALL)
	@echo "BUILT (native): $@"

run: native
	DYLD_FALLBACK_LIBRARY_PATH=$(GLES_LIBS_PATH) \
	LD_LIBRARY_PATH=$(GLES_LIBS_PATH) $(APP)

# ---- headless CA core self-test (no GL; compare with the Python reference:
# python3 ima_traveller_final.py --seed 12345 --frames 12 --checksums) ----
selftest: | $(BIN_DIR)
	$(CC) -O2 $(WARN) ima_core.c ima_selftest.c -o $(BIN_DIR)/ima_selftest
	$(BIN_DIR)/ima_selftest 12345 12

# ---- emscripten ----
# emcc emits traveller.js/.wasm ONLY -- never traveller.html -- so the
# hand-maintained page ($(WEB_PAGE)) survives rebuilds and `make clean`.
browser: $(EM_OUT)

$(EM_OUT): $(SRC) ima_core.h | $(WEB_DIR)
	@test -f "$(EM_FREEGLUT_LIB)" || { echo "ERROR: $(EM_FREEGLUT_LIB) not found -- run ./deps/refresh.sh or set EM_FREEGLUT_LIB."; exit 1; }
	@test -f "$(EM_GL4ES_LIB)" || { echo "ERROR: $(EM_GL4ES_LIB) not found -- run ./deps/refresh.sh or set EM_GL4ES_LIB."; exit 1; }
	$(EMCC) $(EM_OPT) $(WARN) $(EM_INCS) $(EM_SDL_FLAGS) $(SRC) \
		$(EM_FREEGLUT_LIB) $(EM_GL4ES_LIB) \
		$(EM_OUTPUT_FLAGS) \
		-lm -o $@
	@echo "BUILT (web): $@"

run-browser: browser
	emrun $(WEB_PAGE)

clean:
	rm -rf $(BIN_DIR)
	rm -f $(WEB_DIR)/$(APPNAME).js $(WEB_DIR)/$(APPNAME).wasm $(WEB_DIR)/$(APPNAME).data

.PHONY: all native browser selftest run run-browser clean
