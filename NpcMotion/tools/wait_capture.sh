#!/bin/sh
# Waits until a new .rdc appears in captures/ and its size has been stable for 5 s, then prints it and exits.
# Also exits when the game process is gone. Usage: wait_capture.sh [count-of-existing-captures-to-skip]
D=$(cd "$(dirname "$0")/../captures" && pwd)
before=$(ls "$D"/*.rdc 2>/dev/null | sort)
while :; do
  for f in $(ls "$D"/*.rdc 2>/dev/null | sort); do
    case "$before" in *"$f"*) continue ;; esac
    s1=$(stat -c %s "$f"); sleep 5; s2=$(stat -c %s "$f")
    if [ "$s1" = "$s2" ] && [ "$s2" -gt 0 ]; then echo "NEW CAPTURE $f $s2 bytes"; exit 0; fi
  done
  if ! tasklist //FI "IMAGENAME eq ff7rebirth_.exe" 2>/dev/null | grep -q ff7rebirth_; then echo "GAME EXITED"; exit 0; fi
  sleep 3
done
