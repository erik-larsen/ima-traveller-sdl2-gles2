# Makefile -- build "IMA Traveller" (reverse-engineered traveller 2.0c,
# Driessens & Verstappen 1996) for native (Mac/Linux/Win via ANGLE) and the
# browser (Emscripten), on the freeglut SDL2/GLES2/gl4es port.
#
# Same shape as newave.mk in freeglut-sdl2-ogles2/progs/demos/newave, minus
# glues (traveller only uses identity matrices -- no GLU), with the trees
# defaulting to SIBLINGS of this repo (like tkano-rigid-body's rigidbody.mk).
# Links a prebuilt freeglut (Legacy gl4es config, -DFREEGLUT_SDL2_GL4ES=ON)
# + the gl4es you already built.
#
#   make            native build -> ./bin-<os>-<arch>/traveller
#   make run        build + run native
#   make browser    Emscripten   -> ./web/traveller.js
#   make selftest   headless CA core check (no GL)
#   make clean
#
# Point these at your trees (env or make VAR=...):
#   FREEGLUT       = freeglut source (has include/GL/freeglut.h)
#   FREEGLUT_BUILD = freeglut build dir with the built lib (libglut/libfreeglut)
#   GL4ES          = gl4es checkout (include/ + lib/libGL-native.a)
#   OGL_FOR_MAC / GLES_DIR = ANGLE headers + libGLESv2/libEGL

APPNAME = traveller
SRC     = traveller.c ima_core.c

# ---- platform detection (mirrors newave.mk) ----
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

# ---- trees (siblings of this repo; override on the command line or via env) ----
FREEGLUT       = ../freeglut-sdl2-ogles2
FREEGLUT_BUILD = $(FREEGLUT)/build
GL4ES          = ../gl4es
OGL_FOR_MAC    = ../opengl-for-mac
GLES_DIR       = $(OGL_FOR_MAC)

# ---- SDL2 ----
SDL_INC  = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

# ---- freeglut (Legacy gl4es config) ----
FREEGLUT_INC = -I$(FREEGLUT)/include
FREEGLUT_LIBDIR = $(firstword $(wildcard $(FREEGLUT_BUILD)/lib) $(FREEGLUT_BUILD))
FREEGLUT_LIB    = $(firstword \
    $(wildcard $(FREEGLUT_LIBDIR)/libfreeglut.a) \
    $(wildcard $(FREEGLUT_LIBDIR)/libglut.a) \
    $(wildcard $(FREEGLUT_LIBDIR)/libfreeglut.$(DYL_EXT)) \
    $(wildcard $(FREEGLUT_LIBDIR)/libglut.$(DYL_EXT)))

# ---- gl4es (static; headers FIRST so <GL/gl.h> resolves to gl4es') ----
GL4ES_INC = -I$(GL4ES)/include
GL4ES_LIB = $(GL4ES)/lib/libGL-native.a

# ---- ANGLE GLES2 / EGL (static gl4es needs these supplied by the exe) ----
GLES_LIBS_PATH = $(GLES_DIR)/lib
GLES_LIBS      = -L$(GLES_LIBS_PATH) -lGLESv2 -lEGL
GLES_DYLIBS    = libGLESv2.$(DYL_EXT) libEGL.$(DYL_EXT)

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
INCS = $(GL4ES_INC) $(FREEGLUT_INC) $(SDL_INC) -DFREEGLUT_SDL2_GL4ES -DFREEGLUT_STATIC

CC   = cc
EMCC = emcc -s WASM=1

OPT_ZERO = -O0 -g
EM_OPT_ZERO = $(OPT_ZERO) -sASSERTIONS=2 -sGL_ASSERTIONS=1
OPT_TWO = -DNDEBUG -O2
OPT = $(OPT_TWO)
EM_OPT = -DNDEBUG -O2

WARN = -Wall -Wextra -Wno-unused-parameter

EM_SDL_FLAGS    = -s USE_SDL=2 -s FULL_ES2=1
EM_OUTPUT_FLAGS = -s ALLOW_MEMORY_GROWTH=1 -s EXIT_RUNTIME=0

# ---- web (Emscripten) libs ----
EM_FREEGLUT_BUILD ?= $(FREEGLUT)/build-web
EM_FREEGLUT_LIBDIR = $(firstword $(wildcard $(EM_FREEGLUT_BUILD)/lib) $(EM_FREEGLUT_BUILD))
EM_FREEGLUT_LIB    = $(firstword \
    $(wildcard $(EM_FREEGLUT_LIBDIR)/libfreeglut.a) \
    $(wildcard $(EM_FREEGLUT_LIBDIR)/libglut.a))
EM_GL4ES_LIB = $(GL4ES)/lib/libGL-web.a
EM_INCS = $(GL4ES_INC) $(FREEGLUT_INC) -DFREEGLUT_SDL2_GL4ES -DFREEGLUT_STATIC

all: native

native: $(APP)

$(BIN_DIR) $(WEB_DIR):
	mkdir -p $@

# ---- native link ----
# Order: app, freeglut, gl4es (static), then ANGLE GLES/EGL, SDL, libm.
$(APP): $(SRC) ima_core.h | $(BIN_DIR)
	@test -n "$(FREEGLUT_LIB)" || { echo "ERROR: freeglut lib not found under $(FREEGLUT_LIBDIR) -- build freeglut with -DFREEGLUT_SDL2_GL4ES=ON first, or set FREEGLUT_BUILD."; exit 1; }
	@test -f "$(GL4ES_LIB)" || { echo "ERROR: $(GL4ES_LIB) not found -- build gl4es first, or set GL4ES."; exit 1; }
	$(CC) $(OPT) $(WARN) $(INCS) $(SRC) \
		$(FREEGLUT_LIB) $(GL4ES_LIB) \
		$(GLES_LIBS) $(GLES_LINK) $(SDL_LIBS) -lm $(CONSOLE_FLAGS) -o $@
	$(call GLES_INSTALL)
	@echo "BUILT (native): $@"

run: native
	DYLD_FALLBACK_LIBRARY_PATH=$(GL4ES)/lib:$(GLES_LIBS_PATH) \
	LD_LIBRARY_PATH=$(GL4ES)/lib:$(GLES_LIBS_PATH) $(APP)

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
	@test -n "$(EM_FREEGLUT_LIB)" || { echo "ERROR: web freeglut lib not found under $(EM_FREEGLUT_LIBDIR) -- build freeglut for Emscripten first."; exit 1; }
	@test -f "$(EM_GL4ES_LIB)" || { echo "ERROR: $(EM_GL4ES_LIB) not found -- build gl4es for web first."; exit 1; }
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
