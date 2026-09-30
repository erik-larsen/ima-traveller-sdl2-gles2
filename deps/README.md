# Vendored dependencies

Headers and prebuilt libraries so this repo builds standalone.

| Dir | Contents | Source |
|-----|----------|--------|
| `freeglut/` | `include/GL` headers; `lib/native` + `lib/web` `libglut.a` | [freeglut-sdl2-ogles2](https://github.com/sgi-demos/freeglut-sdl2-ogles2) built with `-DFREEGLUT_SDL2_GL4ES=ON` (SDL2 + GLES2 + gl4es backend) |
| `gl4es/` | `include` (GL 1.x/GLU headers); `libGL-native.a`, `libGL-web.a` | [gl4es](https://github.com/ptitSeb/gl4es) (OpenGL 1.x/2.x over GLES2) |
| `angle/lib-mac-arm64/` | `libGLESv2.dylib`, `libEGL.dylib` | Google ANGLE (GLES2 over Metal), from the opengl-for-mac tree |

Native builds also need **SDL2** from the system (`sdl2-config` on PATH,
e.g. `brew install sdl2`); web builds get SDL2 from Emscripten's port.

The ANGLE dylibs are mac-arm64 only. On Linux, GLES2/EGL usually come from
the system (Mesa) — point the build at them with
`make GLES_LIBS_PATH=/usr/lib/...`; on Windows use ANGLE DLLs the same way.

Each dir carries its upstream license (`freeglut/COPYING`,
`gl4es/LICENSE`, `angle/LICENSE`), which must travel with any build that
links these libraries.

To re-vendor after rebuilding the source trees (siblings of this repo by
default): `./refresh.sh`
