#!/bin/zsh
# Launch Shark Tale in a CrossOver bottle, capture the game window, stop it.
#
#   tools/run_dev.sh <label> [seconds=45] [bottle=SharkTaleDev]
#
# Writes docs/runs/<label>/{cx.log,window.png,loaded.txt,exceptions.txt}.
# Only the game window is captured (tools/winid.swift), never the desktop.
set -uo pipefail

ROOT=${0:A:h:h}
LABEL=$1; SECS=${2:-45}; BOTTLE=${3:-SharkTaleDev}
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin
GAME="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/Program Files/Activision/Shark Tale/System"
OUT=$ROOT/docs/runs/$LABEL
WINID=$ROOT/build/tools/winid
mkdir -p $OUT ${WINID:h}
[[ -x $WINID ]] || swiftc -O -o $WINID $ROOT/tools/winid.swift

rm -f "$GAME/Running.ini"
( cd "$GAME" && $CX/wine --bottle $BOTTLE --cx-log $OUT/cx.log --debugmsg '+loaddll,+seh,+file,+debugstr,err+all,fixme-all' \
    -- 'C:\Program Files\Activision\Shark Tale\System\Game.exe' -windowed >/dev/null 2>&1 & )
sleep $SECS

WINCAP=$ROOT/build/tools/wincap
[[ -x $WINCAP ]] || swiftc -O -o $WINCAP $ROOT/tools/wincap.swift
W=$($WINID game | awk -F"\t" '{split($4,a,"x"); print a[1]*a[2] "\t" $1}' | sort -rn | head -1 | cut -f2)
if [[ -n $W ]]; then
  if $WINCAP $W $OUT/window.png 2>/dev/null; then
    sips -Z 900 $OUT/window.png >/dev/null; echo "window $W captured"
  else
    echo "window $W present (screen capture unavailable)"
  fi
else
  echo "no game window found"
fi
pkill -f 'Shark Tale\\System\\Game.exe'
sleep 2

grep -oE 'Loaded L"[^"]*"' $OUT/cx.log | sed 's/.*\\//; s/"$//' | awk '!s[$0]++' > $OUT/loaded.txt
# Game files opened, in order (packages, maps, movies, ini): progress evidence.
grep -oE 'CreateFile[AW]?\(?.{0,4}L?"[^"]*\.(u|utx|uax|usx|ukx|unr|bik|ini|int|ogg)"' $OUT/cx.log \
  | grep -oE '[^\\/"]+\.(u|utx|uax|usx|ukx|unr|bik|ini|int|ogg)' | awk '!s[$0]++' > $OUT/files.txt
echo "opened $(wc -l < $OUT/files.txt) game files; last: $(tail -3 $OUT/files.txt | tr '\n' ' ')"
grep -vE 'trace:file' $OUT/cx.log > $OUT/cx_nofile.log && mv $OUT/cx_nofile.log $OUT/cx.log
grep -E 'dispatch_exception code=' $OUT/cx.log | grep -v 80000026 | sed -E 's/^.*code=/code=/' | sort | uniq -c > $OUT/exceptions.txt
echo "loaded $(wc -l < $OUT/loaded.txt) modules; exceptions:"; cat $OUT/exceptions.txt
