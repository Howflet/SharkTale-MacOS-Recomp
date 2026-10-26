#!/bin/zsh
# Gameplay benchmark: scripted New Game -> LowerCity, headless and muted.
# Prints the mean engine frame rate over the last 60 s of a 220 s run.
#   tools/bench.sh [extra env assignments...]
ROOT=${0:A:h:h}
LOG=$ROOT/build/bench.log
env "$@" SHARK_FPS=1 SHARK_MUTE=1 SHARK_HEADLESS=1 \
  SHARK_SCRIPT="30000:k28,32990:m135/175,33000:c135/175,35990:m320/345,36000:c320/345" \
  perl -e 'alarm 220; exec { $ARGV[0] } @ARGV' "$ROOT/build/sharktale" > $LOG 2>&1
grep -E 'FATAL|recomp\]' $LOG | head -3
awk '/\[fps\]/{v[n++]=$2} END{s=0; k=0; for(i=n-12;i<n;i++) if(i>=0){s+=v[i];k++} if(k) printf "gameplay mean %.1f fps over last %d samples (min/max of window below)\n", s/k, k}' $LOG
grep '\[fps\]' $LOG | tail -12 | awk '{print $2}' | sort -n | sed -n '1p;$p' | tr '\n' ' '; echo
