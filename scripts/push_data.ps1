# Push the extracted game files to the device's app folder.
#   exefs\{rtld,main,subsdk0,sdk}   from the extracted 2.0.0 ExeFS
#   romfs.bin                       from tools\pack_romfs.py
#   spike.on                        enables the native guest run
param(
    [string]$ExeFs = 'S:\Engage-Port\work\engage_2.0.0\exefs',
    [string]$RomFs = 'S:\Engage-Port\work\image\romfs.bin',
    [switch]$SkipRomFs
)
$adb = 'S:\Engage-Port\android-sdk\platform-tools\adb.exe'
$ext = '/sdcard/Android/data/dev.teton.somniel/files'
& $adb shell mkdir -p "$ext/exefs"
foreach ($f in 'rtld', 'main', 'main.npdm', 'subsdk0', 'sdk') {
    & $adb push "$ExeFs\$f" "$ext/exefs/$f" 2>&1 | Select-Object -Last 1
}
if (-not $SkipRomFs) {
    & $adb push $RomFs "$ext/romfs.bin" 2>&1 | Select-Object -Last 1
}
& $adb shell "touch $ext/spike.on"
& $adb shell ls -l $ext
