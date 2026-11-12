#!/bin/bash
# usage: fault_test.sh FLAGS [attempts] - boot until the game reaches the heavy rendering phase (>= 400 submits) or a GPU
# fault shows; restart a run that has not got there after 200 s (the guest boot is slow or deadlocks at random).
A=/s/Engage-Port/platform-tools/adb
export MSYS_NO_PATHCONV=1
FLAGS=${1:-0}
ATT=${2:-5}
D=/sdcard/Android/data/dev.teton.somniel/files
echo -n "$FLAGS" > /s/Engage-Port/work/vkr_flags.txt
$A push "$(cygpath -w /s/Engage-Port/work/vkr_flags.txt)" $D/vkr_flags >/dev/null
$A shell svc power stayon usb >/dev/null
for n in $(seq 1 "$ATT"); do
    $A shell input keyevent KEYCODE_WAKEUP
    $A shell wm dismiss-keyguard >/dev/null
    $A shell am force-stop dev.teton.somniel
    $A shell am start -n dev.teton.somniel/.SomnielActivity >/dev/null
    for i in $(seq 1 20); do
        sleep 10
        LOG=$($A shell "run-as dev.teton.somniel cat files/somniel.log")
        if echo "$LOG" | grep -q "GPU fault"; then echo "FLAGS=$FLAGS try $n: GPU FAULT"; echo "$LOG" | grep -E "GPU fault" | head -2; exit 0; fi
        if echo "$LOG" | grep -q "vkr: submit #400"; then
            sleep 30
            LOG=$($A shell "run-as dev.teton.somniel cat files/somniel.log")
            if echo "$LOG" | grep -q "GPU fault"; then echo "FLAGS=$FLAGS try $n: GPU FAULT"; echo "$LOG" | grep -E "GPU fault" | head -2; exit 0; fi
            echo "FLAGS=$FLAGS try $n: reached heavy phase, NO fault"
            echo "$LOG" | grep -E "presented image" | tail -3
            exit 0
        fi
    done
    echo "try $n: did not reach the heavy phase in 200 s"
done
echo "FLAGS=$FLAGS: gave up"
