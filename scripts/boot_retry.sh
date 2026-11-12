#!/bin/bash
# Start the app and retry until the first frame is submitted (the guest boot sometimes deadlocks, see memory notes).
# usage: boot_retry.sh [max_tries] [wait_seconds] [log_pattern]
A=/s/Engage-Port/platform-tools/adb
TRIES=${1:-5}
WAIT=${2:-75}
PAT=${3:-vkr: submit}
$A shell svc power stayon usb >/dev/null
for n in $(seq 1 "$TRIES"); do
    $A shell input keyevent KEYCODE_WAKEUP
    $A shell wm dismiss-keyguard >/dev/null
    $A shell am force-stop dev.teton.somniel
    $A shell am start -n dev.teton.somniel/.SomnielActivity >/dev/null
    sleep "$WAIT"
    if $A shell "run-as dev.teton.somniel cat files/somniel.log" | grep -q "$PAT"; then
        echo "boot ok on try $n"
        exit 0
    fi
    echo "try $n: no '$PAT' yet, restarting"
done
echo "boot failed after $TRIES tries"
exit 1
