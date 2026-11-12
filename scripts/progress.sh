#!/bin/bash
# usage: progress.sh [runs] [seconds] - start the app N times, report the submit count reached after S seconds and save a screenshot
A=/s/Engage-Port/platform-tools/adb
export MSYS_NO_PATHCONV=1
RUNS=${1:-3}; SEC=${2:-150}
$A shell svc power stayon usb >/dev/null
for n in $(seq 1 "$RUNS"); do
    $A shell input keyevent KEYCODE_WAKEUP; $A shell wm dismiss-keyguard >/dev/null
    $A shell am force-stop dev.teton.somniel
    $A shell am start -n dev.teton.somniel/.SomnielActivity >/dev/null
    sleep "$SEC"
    LOG=$($A shell "run-as dev.teton.somniel cat files/somniel.log")
    echo "run $n: $(echo "$LOG" | grep -oE 'vkr: submit #[0-9]+' | tail -1)  faults: $(echo "$LOG" | grep -c 'GPU fault')"
    echo "$LOG" | grep "presented image" | tail -1
    $A exec-out screencap -p > scripts/prog_$n.png
done
