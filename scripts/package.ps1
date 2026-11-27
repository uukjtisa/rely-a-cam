<#
.SYNOPSIS
  Builds both halves and packages a release: RelyACam-Setup-<ver>.exe and RelyACam-<ver>.apk in release\.

.DESCRIPTION
  1. Checks the plugin version (obs-plugin\buildspec.json) and the app version
     (android\app\build.gradle.kts) agree: one release tag covers both halves.
  2. Builds the plugin (CMake, RelWithDebInfo) and the signed release APK (Gradle).
  3. Stages the plugin folder: the DLL, its locale, the APK (served to phones from the PC and
     installed over USB), and adb from the Android SDK's platform-tools (Apache-2.0).
  4. Compiles installer\RelyACam.iss with Inno Setup.

.PARAMETER SkipBuild
  Package what is already built.

.PARAMETER GesturesSetup
  Rely-A-Cam-Gestures' own setup (RelyACamGestures-Setup-<ver>.exe), bundled behind an unticked
  "Also install Rely-A-Cam-Gestures" box. Defaults to the RACG_SETUP environment variable; with
  neither, the installer has no such box.
#>
param([switch]$SkipBuild, [string]$GesturesSetup = $env:RACG_SETUP)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$ver = (Get-Content "$root\obs-plugin\buildspec.json" -Raw | ConvertFrom-Json).version
$gradle = Get-Content "$root\android\app\build.gradle.kts" -Raw
if ($gradle -notmatch "versionName = `"$([regex]::Escape($ver))`"") {
    throw "Version mismatch: buildspec.json says $ver; set versionName (and bump versionCode) in android\app\build.gradle.kts to match."
}
Write-Host "Packaging Rely-A-Cam $ver"

# Android SDK and a JDK: local.properties first, then the environment.
$sdk = $env:ANDROID_HOME
$lp = "$root\android\local.properties"
if (Test-Path $lp) {
    $line = Select-String -Path $lp -Pattern '^sdk\.dir=(.*)$' | Select-Object -First 1
    if ($line) { $sdk = $line.Matches[0].Groups[1].Value -replace '\\:', ':' }
}
if (-not $sdk -or -not (Test-Path "$sdk\platform-tools\adb.exe")) { throw "Android SDK not found (set sdk.dir in android\local.properties)." }
if (-not $env:JAVA_HOME) {
    foreach ($j in @('D:\Program Files\Android-Studio\jbr', "$env:ProgramFiles\Android\Android Studio\jbr")) {
        if (Test-Path "$j\bin\java.exe") { $env:JAVA_HOME = $j; break }
    }
}

if (-not $SkipBuild) {
    if (-not (Test-Path "$root\obs-plugin\build_x64")) {
        Push-Location "$root\obs-plugin"; cmake --preset windows-x64; Pop-Location
    }
    cmake --build "$root\obs-plugin\build_x64" --config RelWithDebInfo --parallel
    if ($LASTEXITCODE) { throw "Plugin build failed." }
    & "$root\obs-plugin\build_x64\RelWithDebInfo\rac-protocol-test.exe"
    if ($LASTEXITCODE) { throw "Protocol vectors failed (plugin side)." }
    Push-Location "$root\android"
    & .\gradlew.bat :core:protocol:test :app:assembleRelease
    $code = $LASTEXITCODE
    Pop-Location
    if ($code) { throw "Android build failed." }
}

$apk = "$root\android\app\build\outputs\apk\release\RelyACam-release.apk"
if (-not (Test-Path $apk)) { throw "No signed release APK at $apk (is keystore.properties set up?)." }
$dll = "$root\obs-plugin\build_x64\RelWithDebInfo\rely-a-cam.dll"
if (-not (Test-Path $dll)) { throw "No plugin DLL at $dll." }

$out = "$root\release"
$stage = "$out\stage\rely-a-cam"
if (Test-Path "$out\stage") { Remove-Item -Recurse -Force "$out\stage" }
New-Item -ItemType Directory -Force "$stage\bin\64bit", "$stage\data\adb", "$stage\data\licenses" | Out-Null

Copy-Item $dll "$stage\bin\64bit\"
Copy-Item -Recurse "$root\obs-plugin\data\locale" "$stage\data\"
Copy-Item $apk "$stage\data\RelyACam.apk"
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'libwinpthread-1.dll') {
    if (Test-Path "$sdk\platform-tools\$f") { Copy-Item "$sdk\platform-tools\$f" "$stage\data\adb\" }
}
if (Test-Path "$sdk\platform-tools\NOTICE.txt") { Copy-Item "$sdk\platform-tools\NOTICE.txt" "$stage\data\licenses\adb-NOTICE.txt" }
Copy-Item "$root\obs-plugin\LICENSE" "$stage\data\licenses\rely-a-cam-plugin-GPL-2.0.txt"
Copy-Item "$root\obs-plugin\src\third_party\qrcodegen\LICENSE.txt" "$stage\data\licenses\qrcodegen-MIT.txt" -ErrorAction SilentlyContinue

Copy-Item $apk "$out\RelyACam-$ver.apk" -Force

$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    foreach ($p in "$env:USERPROFILE\scoop\apps\inno-setup\current\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 7\ISCC.exe") {
        if (Test-Path $p) { $iscc = $p; break }
    }
}
if (-not $iscc) { throw "Inno Setup not found (scoop install inno-setup)." }
$defs = @("/DAppVersion=$ver", "/DStage=$stage", "/DOutDir=$out")
if ($GesturesSetup) {
    if (-not (Test-Path $GesturesSetup)) { throw "No gestures setup at $GesturesSetup." }
    $defs += "/DGesturesSetup=$((Resolve-Path $GesturesSetup).Path)"
    Write-Host "Bundling $(Split-Path -Leaf $GesturesSetup)"
}
& $iscc /Q @defs "$root\installer\RelyACam.iss"
if ($LASTEXITCODE) { throw "Installer build failed." }

Write-Host ""
Write-Host "Done:"
Write-Host "  $out\RelyACam-Setup-$ver.exe"
Write-Host "  $out\RelyACam-$ver.apk"
