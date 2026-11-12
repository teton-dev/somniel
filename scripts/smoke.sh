#!/usr/bin/env bash
# smoke.sh app.apk -- install, launch, stay alive 45 s, debug socket answers.
# Prints "SMOKE: PASS" or "SMOKE: FAIL (reason)". Needs adb and one device.
set -u
APK="${1:?usage: smoke.sh app.apk}"
PKG=dev.teton.somniel
ACT=$PKG/.SomnielActivity
ALIVE_SECS="${ALIVE_SECS:-45}"
ADB="${ADB:-adb}"

fail() { echo "SMOKE: FAIL ($1)"; exit 1; }

"$ADB" get-state >/dev/null 2>&1 || fail "no device"
"$ADB" install -r -t "$APK" >/dev/null 2>&1 || fail "install"
"$ADB" shell am force-stop $PKG
"$ADB" logcat -c
"$ADB" shell am start -n $ACT >/dev/null 2>&1 || fail "launch"

sleep 5
"$ADB" shell pidof $PKG >/dev/null 2>&1 || fail "process not running after 5 s"

PORT=7777
"$ADB" forward tcp:$PORT localabstract:somniel_debug >/dev/null
reply=""
for _ in 1 2 3 4 5; do
    reply=$( { exec 3<>/dev/tcp/127.0.0.1/$PORT && echo '{"cmd":"ping"}' >&3 && timeout 3 head -n1 <&3; } 2>/dev/null )
    [ -n "$reply" ] && break
    sleep 1
done
case "$reply" in *pong*) ;; *) fail "debug socket did not answer ping";; esac

sleep $((ALIVE_SECS - 5))
"$ADB" shell pidof $PKG >/dev/null 2>&1 || fail "process died before ${ALIVE_SECS} s"
if "$ADB" logcat -d | grep -q "Fatal signal"; then fail "native crash in logcat"; fi

echo "SMOKE: PASS"
