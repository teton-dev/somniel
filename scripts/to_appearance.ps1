# Launch the installed app (screen kept on), wait for the opening movie, then press through the title, the title menu
# and the New Game flow to the Appearance screen; take a screenshot.
# usage: to_appearance.ps1 [-Shot path.png] [-Install]
param([string]$Shot = "S:\Engage-Port\work\appearance.png", [switch]$Install)
$adb = 'S:\Engage-Port\android-sdk\platform-tools\adb.exe'
$pkg = 'dev.teton.somniel'
if ($Install) { & $adb install -r -t S:\Engage-Port\somniel\app\build\outputs\apk\debug\app-debug.apk | Select-Object -Last 1 }
& $adb shell svc power stayon usb
& $adb shell input keyevent KEYCODE_WAKEUP
& $adb shell wm dismiss-keyguard
& $adb shell am force-stop $pkg
& $adb shell am start -n "$pkg/.SomnielActivity" | Out-Null
$t0 = Get-Date
$ok = $false
while (((Get-Date) - $t0).TotalSeconds -lt 240) {
    Start-Sleep -Seconds 2
    $l = & $adb shell run-as $pkg cat files/somniel.log | Select-String 'vp9 output 3 |CRASH'
    if ($l) { $ok = -not ($l -match 'CRASH'); break }
}
if (-not $ok) { "did not reach the movie"; exit 1 }
"movie after $([int]((Get-Date) - $t0).TotalSeconds) s"
& "$PSScriptRoot\press.ps1" "wait 2000 A wait 5000 A wait 6000 A wait 2500 A wait 2500 A wait 2500 A wait 2500 A wait 2500 A" -Shot $Shot -After 3
"screenshot: $Shot"
