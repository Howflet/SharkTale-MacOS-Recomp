#!/bin/zsh
# Full native rebuild: emit C from Ghidra, compile every module, link build/sharktale.
#   tools/recomp_build_all.sh [Module.dll ...]   (default: all modules)
set -uo pipefail
ROOT=${0:A:h:h}
$ROOT/tools/recomp_emit_all.sh "$@"
mods=()
if (( $# )); then for f in "$@"; do mods+=(${f:r}); done
else mods=($(grep -v '^#' $ROOT/tools/recomp_layout.txt | awk 'NF{print $1}' | sed 's/\.[^.]*$//')); fi
for m in $mods; do $ROOT/tools/recomp_compile.sh $m 10 | head -3; done
$ROOT/tools/recomp_link.sh 2>&1 | grep -E 'error|linked|Undefined'
