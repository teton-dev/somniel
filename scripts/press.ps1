# Play a button script in the running app (see main.c script_poll) and take a screenshot afterwards.
# usage: press.ps1 "A wait 1500 A DOWN A" [-Shot path.png] [-After seconds]
param([Parameter(Mandatory = $true)][string]$Script, [string]$Shot = "", [double]$After = 1.5)
$adb = 'S:\Engage-Port\android-sdk\platform-tools\adb.exe'
$ext = '/storage/emulated/0/Android/data/dev.teton.somniel/files'
$tmp = Join-Path $env:TEMP 'somniel_input.txt'
($Script -split ' ' | Where-Object { $_ }) -join "`n" | Set-Content -Encoding ascii -NoNewline $tmp
$env:MSYS_NO_PATHCONV = 1
& $adb push $tmp "$ext/input.txt" | Out-Null
& $adb shell touch "$ext/input.txt"
# total script time: holds and waits plus the 200 ms gaps
$ms = 0
$t = $Script -split ' ' | Where-Object { $_ }
for ($i = 0; $i -lt $t.Count; $i++) {
    if ($t[$i] -eq 'wait') { $ms += [int]$t[$i + 1]; $i++ }
    elseif ($i + 1 -lt $t.Count -and $t[$i + 1] -match '^\d+$') { $ms += [int]$t[$i + 1] + 200; $i++ }
    else { $ms += 400 }
}
Start-Sleep -Milliseconds ($ms + 600 + [int]($After * 1000))
if ($Shot) { cmd /c "`"$adb`" exec-out screencap -p > $Shot" }
