#!/bin/bash
# Cross-compile the multiplayer executables for Windows x86-64 on Ubuntu/WSL.
#
#   sudo apt install g++-mingw-w64-x86-64
#   tools/multiplayer/build-windows-mingw.sh [BUILD_DIR=build-win] [--reconfigure]
#
# Run from the source root. Output: BUILD_DIR/src/multiplayer/*.exe (static; only
# Windows system DLLs and the Universal CRT are imported).
#
# The upstream tpt-libs MinGW pack is built by MSYS2 against the Universal CRT,
# while Ubuntu's MinGW defaults to msvcrt. So:
#   * a specs file links -lucrt instead of -lmsvcrt (GCC 13 lacks -mcrtdll);
#   * headers stay msvcrt-style: -D_UCRT would change the mangled names of
#     libstdc++ symbols (mbstate_t) and break linking with Ubuntu's libstdc++;
#   * the win32 thread model is used: Ubuntu's winpthreads is msvcrt-only;
#   * a small shim supplies five symbols the prebuilt libraries expect from
#     MSYS2's UCRT import libraries (_setjmp, two UCRT header inlines, and the
#     64-bit fseeko/ftello imports).
set -euo pipefail
BUILD=${1:-build-win}
[ "${1:-}" = "--reconfigure" ] && BUILD=build-win
RECONFIGURE=0
for arg in "$@"; do [ "$arg" = "--reconfigure" ] && RECONFIGURE=1; done
TOOLDIR=$(realpath -m "$BUILD-toolchain")
mkdir -p "$TOOLDIR"
SPECS=$TOOLDIR/ucrt.specs
SHIM=$TOOLDIR/ucrt-shim.o
x86_64-w64-mingw32-g++-win32 -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > "$SPECS"

cat > "$TOOLDIR/setjmp.S" <<'EOF'
    # UCRT exports _setjmp as __intrinsic_setjmp. A tail jump keeps the
    # caller's frame and return address, as setjmp requires.
    .text
    .globl _setjmp
_setjmp:
    jmp __intrinsic_setjmp
    .section .rdata,"dr"
    .globl __imp__setjmp
    .p2align 3
__imp__setjmp:
    .quad _setjmp
EOF
cat > "$TOOLDIR/ucrt-shim.c" <<'EOF'
#include <stdio.h>
/* UCRT header inlines that the prebuilt mbedtls references out of line. */
unsigned long long *__local_stdio_printf_options(void) { static unsigned long long o; return &o; }
unsigned long long *__local_stdio_scanf_options(void) { static unsigned long long o; return &o; }
/* SDL2 imports the MSYS2 UCRT names for 64-bit file offsets. */
static int Seek64(FILE *f, long long offset, int whence) { return _fseeki64(f, offset, whence); }
static long long Tell64(FILE *f) { return _ftelli64(f); }
int (*__imp_fseeko64)(FILE *, long long, int) = Seek64;
long long (*__imp_ftello64)(FILE *) = Tell64;
EOF
x86_64-w64-mingw32-gcc-win32 -c "$TOOLDIR/setjmp.S" -o "$TOOLDIR/setjmp.o"
x86_64-w64-mingw32-gcc-win32 -O2 -c "$TOOLDIR/ucrt-shim.c" -o "$TOOLDIR/ucrt-shim-c.o"
x86_64-w64-mingw32-ld -r "$TOOLDIR/setjmp.o" "$TOOLDIR/ucrt-shim-c.o" -o "$SHIM"

cat > "$TOOLDIR/cross.ini" <<EOF
[binaries]
c = 'x86_64-w64-mingw32-gcc-win32'
cpp = 'x86_64-w64-mingw32-g++-win32'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[built-in options]
c_args = ['-specs=$SPECS']
cpp_args = ['-specs=$SPECS']
c_link_args = ['-specs=$SPECS', '-static', '$SHIM']
cpp_link_args = ['-specs=$SPECS', '-static', '$SHIM']

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

if [ ! -f "$BUILD/build.ninja" ] || [ "$RECONFIGURE" = 1 ]; then
  rm -rf "$BUILD"
  meson setup "$BUILD" --cross-file "$TOOLDIR/cross.ini" --buildtype=release -Dstatic=prebuilt \
    -Dlua=none -Dhttp=false -Dbuild_multiplayer=true -Dresolve_vcs_tag=no
fi
M=src/multiplayer
ninja -C "$BUILD" powder.exe $M/tpt-server.exe $M/tpt-remote.exe $M/tpt-replica.exe \
  $M/multiplayer-tests.exe $M/tpt-replica-tests.exe $M/tpt-checkpoint-tests.exe $M/tpt-rollback-benchmark.exe
ls -la "$BUILD"/powder.exe "$BUILD"/$M/*.exe
