#!/bin/zsh
# Build a rebuilt module as a 32-bit Windows DLL that drops in next to the
# original game binaries.
#
#   tools/build_module.sh <Module> <imported modules...>
#   e.g. tools/build_module.sh StubAudio Core Engine
#
# Sources: src/<Module>/Src/*.cpp plus src/Runtime/DllCrt.cpp.
# Output:  build/<Module>/<Module>.dll (+ .lib/.map).
set -euo pipefail

ROOT=${0:A:h:h}
LLVM=/opt/homebrew/opt/llvm/bin
LLD=/opt/homebrew/opt/lld/bin
MODULE=$1; shift
OUT=$ROOT/build/$MODULE
mkdir -p $OUT/obj

INCLUDES=()
for d in $ROOT/src/*/Inc(N/); do INCLUDES+=("-I$d"); done

CFLAGS=(
  --target=i686-pc-windows-msvc /std:c++17 /c /O2 /Oy-
  /GR- /GS- /EHs-c- /Zl /Zp4 /Zc:wchar_t- /Zc:sizedDealloc- /arch:SSE2
  -Xclang -fno-access-control
  -Wno-microsoft-enum-forward-reference -Wno-ignored-attributes
  -Wno-invalid-noreturn -Wno-pragma-pack -Wno-unused-value
  "${INCLUDES[@]}"
  ${=SHARK_CFLAGS:-}
)

OBJS=()
for src in $ROOT/src/$MODULE/Src/*.cpp $ROOT/src/Runtime/DllCrt.cpp; do
  obj=$OUT/obj/${src:t:r}.obj
  $LLVM/clang-cl $CFLAGS /Fo$obj -- $src
  OBJS+=($obj)
done

LIBS=()
for m in "$@" msvcr70 ${=SHARK_LIBS:-}; do LIBS+=($ROOT/build/implib/$m.lib); done

$LLD/lld-link /dll /machine:x86 /nodefaultlib /entry:_DllMainCRTStartup \
  /subsystem:windows /base:0x10000000 /out:$OUT/$MODULE.dll /map:$OUT/$MODULE.map \
  $OBJS $LIBS
echo "built $OUT/$MODULE.dll"
