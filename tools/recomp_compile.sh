#!/bin/zsh
# Compile one recompiled module's generated C to objects (parallel).
#   tools/recomp_compile.sh <Module> [jobs=8]
# Objects: build/recomp/obj/<Module>/*.o ; per-file errors in *.err.
set -uo pipefail
ROOT=${0:A:h:h}
MODULE=$1; JOBS=${2:-8}
SRC=$ROOT/build/recomp/$MODULE
OBJ=$ROOT/build/recomp/obj/$MODULE
mkdir -p $OBJ

# Functions replaced natively (tools/native_overrides.txt): rename their
# generated definitions to <name>_recomp.
typeset -A DEFS
while read -r mod fn _; do
  [[ $mod == $MODULE && -n $fn ]] || continue
  def=$(grep -l "^void $fn(Ctx\* restrict c) {" $SRC/*.c | head -1)
  [[ -n $def ]] && DEFS[$def]+=" -D$fn=${fn}_recomp"
done < <(grep -v '^#' $ROOT/tools/native_overrides.txt)

compile_one() {
  local f=$1 b=${1:t:r}
  /usr/bin/clang -c -O2 -w ${=DEFS[$f]:-} -I$ROOT/src/Recomp -I$SRC -o $OBJ/$b.o $f 2> $OBJ/$b.err
  echo "$b $?"
}

failed=0
for f in $SRC/*.c; do
  compile_one $f &
  while (( $(jobs -r | wc -l) >= JOBS )); do sleep 0.1; done
done > $OBJ/status.txt
wait
failed=$(awk '$2!=0' $OBJ/status.txt | wc -l | tr -d ' ')
echo "$MODULE: $(wc -l < $OBJ/status.txt | tr -d ' ') files, $failed failed"
cat $OBJ/*.err 2>/dev/null | grep -E 'error' | sed -E 's/^[^:]+:[0-9]+:[0-9]+: //' | sort | uniq -c | sort -rn | head -15
