#!/bin/zsh
# Build the runtime and link the native executable build/sharktale.
set -euo pipefail
ROOT=${0:A:h:h}
OBJ=$ROOT/build/recomp/runtime_obj
mkdir -p $OBJ
(cd $ROOT/src/Recomp && xxd -i gfx_shaders.metal) > $OBJ/gfx_shaders.h
for f in $ROOT/src/Recomp/*.m; do
  /usr/bin/clang -c -O2 -g -fobjc-arc -Wall -I$ROOT/src/Recomp -I$OBJ -o $OBJ/${f:t:r}.o $f
done
for f in $ROOT/src/Recomp/*.c; do
  /usr/bin/clang -c -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -I$ROOT/src/Recomp -I/opt/homebrew/include -o $OBJ/${f:t:r}.o $f
done
/usr/bin/clang -o $ROOT/build/sharktale $OBJ/*.o $ROOT/build/recomp/obj/*/*.o(N) -lm -framework Metal -framework QuartzCore -framework AppKit -framework ImageIO -framework UniformTypeIdentifiers -framework OpenAL -framework GameController
echo "linked $ROOT/build/sharktale ($(du -h $ROOT/build/sharktale | cut -f1))"
