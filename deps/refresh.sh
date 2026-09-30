#!/bin/sh
# Re-vendor the headers and prebuilt libs under deps/ from sibling checkouts.
#
#   freeglut-sdl2-ogles2  built with -DFREEGLUT_SDL2_GL4ES=ON (build/ + build-web/)
#   gl4es                 built for native + web (lib/libGL-native.a, libGL-web.a)
#   opengl-for-mac        ANGLE dylibs for mac (libGLESv2/libEGL)
#
# Override the tree locations via env: FREEGLUT=... GL4ES=... OGL_FOR_MAC=...
set -e
cd "$(dirname "$0")"

FREEGLUT=${FREEGLUT:-../../freeglut-sdl2-ogles2}
GL4ES=${GL4ES:-../../gl4es}
OGL_FOR_MAC=${OGL_FOR_MAC:-../../opengl-for-mac}

rm -rf freeglut/include gl4es/include
mkdir -p freeglut/include freeglut/lib/native freeglut/lib/web \
         gl4es/lib angle/lib-mac-arm64

cp -R "$FREEGLUT/include/." freeglut/include/
cp "$FREEGLUT/COPYING" freeglut/
cp "$FREEGLUT/build/lib/libglut.a"     freeglut/lib/native/
cp "$FREEGLUT/build-web/lib/libglut.a" freeglut/lib/web/

cp -R "$GL4ES/include/." gl4es/include/
cp "$GL4ES/LICENSE" gl4es/
cp "$GL4ES/lib/libGL-native.a" "$GL4ES/lib/libGL-web.a" gl4es/lib/

cp "$OGL_FOR_MAC/lib/libGLESv2.dylib" "$OGL_FOR_MAC/lib/libEGL.dylib" \
   angle/lib-mac-arm64/

echo "deps refreshed:"
find . -name "*.a" -o -name "*.dylib" | sort
