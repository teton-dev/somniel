# Launch the installed app, skip the opening movie, pass the title screen to the title menu, take a screenshot and
# (unless -NoCapture) a render-target capture, pull it and convert it to PNGs (tools/rtdump.py).
# usage: menu_capture.ps1 [-Out DIR] [-Install] [-NoCapture] [-ExtraKeys "KEYCODE_DPAD_DOWN KEYCODE_ENTER"]
param([string]$Out = "S:\Engage-Port\work\cap", [switch]$Install, [switch]$NoCapture, [string]$ExtraKeys = "")
$adb = 'S:\Engage-Port\android-sdk\platform-tools\adb.exe'
$pkg = 'dev.teton.somniel'
$ext = "/storage/emulated/0/Android/data/$pkg/files"
if ($Install) { & $adb install -r -t S:\Engage-Port\somniel\app\build\outputs\apk\debug\app-debug.apk | Select-Object -Last 1 }
& $adb shell svc power stayon usb
& $adb shell input keyevent KEYCODE_WAKEUP
& $adb shell wm dismiss-keyguard
& $adb shell am force-stop $pkg
& $adb shell am start -n "$pkg/.SomnielActivity" | Out-Null
$t0 = Get-Date
$ok = $false
while (((Get-Date) - $t0).TotalSeconds -lt 180) {
    Start-Sleep -Seconds 2
    $l = & $adb shell run-as $pkg cat files/somniel.log | Select-String 'vp9 output 3 |CRASH'
    if ($l) { $ok = -not ($l -match 'CRASH'); break }
}
if (-not $ok) { "did not reach the movie"; exit 1 }
Start-Sleep 3; & $adb shell input keyevent KEYCODE_ENTER   # skip the movie
Start-Sleep 5; & $adb shell input keyevent KEYCODE_ENTER   # pass the title screen
Start-Sleep 8
foreach ($k in ($ExtraKeys -split ' ' | Where-Object { $_ })) { & $adb shell input keyevent $k; Start-Sleep 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
cmd /c "`"$adb`" exec-out screencap -p > $Out\screen.png"
if ($NoCapture) { exit 0 }
& $adb shell touch "$ext/dump_rt"
Start-Sleep 12
$raw = Join-Path $Out 'raw'
New-Item -ItemType Directory -Force $raw | Out-Null
$env:MSYS_NO_PATHCONV = 1
& $adb pull "$ext/rtdump/." $raw | Select-Object -Last 1
python -I S:\Engage-Port\somniel\tools\rtdump.py $raw (Join-Path $Out 'png')
& $adb shell run-as $pkg cat files/somniel.log | Select-String '^cap ' | Out-File -Encoding utf8 (Join-Path $Out 'passes.txt')
"passes: $((Get-Content (Join-Path $Out 'passes.txt')).Count) lines in $Out\passes.txt"
