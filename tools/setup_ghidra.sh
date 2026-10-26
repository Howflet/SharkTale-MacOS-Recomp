#!/bin/zsh
# Recreate the Ghidra project the recompiler reads (ghidra/SharkTale) from the
# original binaries in orig/System.
#   tools/setup_ghidra.sh [project-dir]     (default: ghidra/)
#
# 1. Checks orig/System against docs/SHA256SUMS (the generated code is specific
#    to those exact binaries; SKIP_HASH_CHECK=1 to override).
# 2. Imports the modules in tools/recomp_layout.txt with Ghidra's default
#    auto-analysis.
# 3. Runs the fix-up scripts, in the order the original project was built:
#      PropagateThunkNames      export names onto bodies behind jmp stubs
#      ApplyLayouts             class/struct layouts (symbols/layouts.json)
#      FixVarargs               printf-style signatures
#      CreateDataReferencedFunctions  functions reached only from data tables
#      CreateStubTargets        incremental-link thunk tables
#      PropagateThunkNames      again, for the new stubs
# Takes about 10 minutes on an M-series Mac. Refuses to touch an existing project.
set -euo pipefail
ROOT=${0:A:h:h}
DIR=${1:-$ROOT/ghidra}
ORIG=$ROOT/orig/System
SCRIPTS=$ROOT/tools/ghidra_scripts

GH=""
for cand in "$(brew --prefix ghidra 2>/dev/null)/libexec/support/analyzeHeadless" \
            /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless \
            /usr/local/opt/ghidra/libexec/support/analyzeHeadless; do
  [[ -x $cand ]] && { GH=$cand; break; }
done
[[ -n $GH ]] || { echo "Ghidra not found: brew install ghidra openjdk@21" >&2; exit 1; }

if [[ -e $DIR/SharkTale.gpr ]]; then
  echo "$DIR/SharkTale.gpr already exists; remove it or pass another directory" >&2
  exit 1
fi

typeset -a mods files
mods=($(grep -v '^#' $ROOT/tools/recomp_layout.txt | awk 'NF{print $1}'))
for m in $mods; do
  [[ -f $ORIG/$m ]] || { echo "missing $ORIG/$m (copy your installed System folder to orig/System)" >&2; exit 1; }
  files+=("$ORIG/$m")
done

if [[ -z ${SKIP_HASH_CHECK:-} ]]; then
  bad=0
  for m in $mods; do
    want=$(awk -v f="$m" '$2==f{print $1}' $ROOT/docs/SHA256SUMS)
    [[ -z $want ]] && continue
    have=$(shasum -a 256 "$ORIG/$m" | awk '{print $1}')
    if [[ $have != $want ]]; then
      echo "hash mismatch: $m" >&2
      bad=1
    fi
  done
  (( bad )) && { echo "orig/System differs from docs/SHA256SUMS (SKIP_HASH_CHECK=1 to continue anyway)" >&2; exit 1; }
  echo "orig/System matches docs/SHA256SUMS"
fi

mkdir -p $DIR
echo "importing and analysing ${#mods} modules (several minutes)..."
$GH $DIR SharkTale -import $files -scriptPath $SCRIPTS 2>&1 \
  | grep -E 'Import succeeded|ERROR' | sed 's/.*INFO  //'

echo "running fix-up scripts..."
$GH $DIR SharkTale -process -noanalysis -scriptPath $SCRIPTS \
  -postScript PropagateThunkNames.java \
  -postScript ApplyLayouts.java $ROOT/symbols/layouts.json \
  -postScript FixVarargs.java \
  -postScript CreateDataReferencedFunctions.java \
  -postScript CreateStubTargets.java \
  -postScript PropagateThunkNames.java 2>&1 \
  | grep -E 'Renamed|Applied|Fixed|Created|Stub tables|ERROR|Exception' | sed 's/.*> //'

echo "Ghidra project ready: $DIR/SharkTale.gpr"
