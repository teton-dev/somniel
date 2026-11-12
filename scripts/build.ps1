# Builds the debug APK with the local toolchain under S:\Engage-Port.
# Gradle 8.14 needs JDK 21 (the system Java 27 is too new for it).
$root = 'S:\Engage-Port'
$jdk = (Get-ChildItem "$root\jdk21" -Directory | Select-Object -First 1).FullName
$env:JAVA_HOME = $jdk
$env:ANDROID_HOME = "$root\android-sdk"
$env:PATH = "$jdk\bin;$root\ninja-win;$env:PATH"
Push-Location "$PSScriptRoot\.."
try {
    & "$root\gradle\gradle-8.14.3\bin\gradle.bat" :app:assembleDebug --console=plain @args
} finally {
    Pop-Location
}
