<#
.SYNOPSIS
  Packages a release and puts it on GitHub: one release, both halves (plugin installer + APK).

.DESCRIPTION
  Runs package.ps1, tags v<version>, pushes the tag, creates the GitHub release and uploads
  RelyACam-Setup-<ver>.exe and RelyACam-<ver>.apk. Both updaters (the plugin's and the app's) read
  this release, and each half's "get the other one" link points at releases/latest.

  By default the release is a DRAFT: nothing is public until you press Publish on GitHub, or
  run again with -Publish. The repository's own visibility is never changed here.

  Uses the GitHub login git already saved (git credential fill); the token is never printed.

.PARAMETER Publish
  Publish the release instead of leaving a draft.
.PARAMETER SkipBuild
  Upload what package.ps1 built last time.
#>
param([switch]$Publish, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$repo = 'uukjtisa/rely-a-cam'

if ($SkipBuild) { & "$PSScriptRoot\package.ps1" -SkipBuild } else { & "$PSScriptRoot\package.ps1" }
$ver = (Get-Content "$root\obs-plugin\buildspec.json" -Raw | ConvertFrom-Json).version
$tag = "v$ver"
$exe = "$root\release\RelyACam-Setup-$ver.exe"
$apk = "$root\release\RelyACam-$ver.apk"

Push-Location $root
try {
    if (git status --porcelain --untracked-files=no) { throw "Commit your changes first: the tag must match what was built." }
    if (-not (git tag --list $tag)) { git tag -a $tag -m "Rely-A-Cam $ver" }
    git push origin $tag
} finally { Pop-Location }

$cred = "protocol=https`nhost=github.com`n`n" | git credential fill
$token = ($cred | Select-String '^password=(.*)$').Matches[0].Groups[1].Value
if (-not $token) { throw "git has no saved GitHub login. Push once with git to save it." }
$h = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'User-Agent' = 'rely-a-cam-release' }

$notes = @"
Rely-A-Cam $ver

**PC:** download **RelyACam-Setup-$ver.exe** and run it. It finds OBS, installs the plugin and lets phones through Windows Firewall. Open OBS: the Phone Camera dock shows a QR.

**Phone:** download **RelyACam-$ver.apk** on the phone, or scan the dock's "No app on the phone?" QR to get it straight from your PC, or plug the phone in and click "Install app over USB".

Both halves update themselves from this page. See SETUP.md for pictures.
"@
$body = @{ tag_name = $tag; name = "Rely-A-Cam $ver"; body = $notes; draft = (-not $Publish); prerelease = $false } | ConvertTo-Json
$rel = Invoke-RestMethod -Method Post -Uri "https://api.github.com/repos/$repo/releases" -Headers $h -Body $body
foreach ($f in $exe, $apk) {
    $name = Split-Path $f -Leaf
    $type = if ($name.EndsWith('.apk')) { 'application/vnd.android.package-archive' } else { 'application/vnd.microsoft.portable-executable' }
    $url = "https://uploads.github.com/repos/$repo/releases/$($rel.id)/assets?name=$name"
    Invoke-RestMethod -Method Post -Uri $url -Headers ($h + @{ 'Content-Type' = $type }) -InFile $f | Out-Null
    Write-Host "uploaded $name"
}
$state = if ($Publish) { 'published' } else { 'saved as a DRAFT (publish it on GitHub when ready)' }
Write-Host "Release $tag $state : $($rel.html_url)"
