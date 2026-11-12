# Build, install, run the native-execution spike on the connected device and
# show the log. Needs exefs and spike.on already pushed (see push_exefs below).
param([switch]$NoBuild, [int]$Wait = 10, [int]$TraceTail = 25, [switch]$KeepScreenOn)
$adb = 'S:\Engage-Port\android-sdk\platform-tools\adb.exe'
$pkg = 'dev.teton.somniel'
$apk = "$PSScriptRoot\..\app\build\outputs\apk\debug\app-debug.apk"
if (-not $NoBuild) {
    $out = & "$PSScriptRoot\build.ps1" 2>&1 | Out-String
    if ($out -notmatch 'BUILD SUCCESSFUL') {
        ($out -split "`n" | Select-String -Pattern 'error:|undefined|FAILED' | Select-Object -First 20) -join "`n"
        throw 'build failed'
    }
}
$ErrorActionPreference = 'Continue'
& $adb install -r -t $apk 2>&1 | Select-Object -Last 1
& $adb shell input keyevent KEYCODE_WAKEUP
& $adb shell am force-stop $pkg
& $adb logcat -c
& $adb shell am start -n "$pkg/.SomnielActivity" | Out-Null
Start-Sleep $Wait
"--- somniel.log (without fps lines)"
& $adb shell run-as $pkg cat files/somniel.log 2>&1 | Where-Object { $_ -notmatch '^fps ' }
"--- svc_trace.log (last $TraceTail)"
& $adb shell run-as $pkg cat files/svc_trace.log 2>&1 | Select-Object -Last $TraceTail
"--- crash in logcat?"
& $adb logcat -d 2>&1 | Select-String -Pattern 'Fatal signal|SIGSEGV|SIGILL|SIGBUS' | Select-Object -First 5

# Burn-in care: the logs are read, so stop the app and put the panel to sleep (-KeepScreenOn to skip).
if (-not $KeepScreenOn) {
    & $adb shell am force-stop $pkg
    & $adb shell input keyevent KEYCODE_SLEEP
    'screen off (OLED burn-in care)'
}
