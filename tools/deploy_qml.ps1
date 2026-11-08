param(
    [string]$Exe = 'build-qml/bin/envhosts.exe',
    [string]$Out = 'build-qml/deploy',
    [Parameter(Mandatory = $true)][string]$QtDir,
    [string]$VcRedistDir,
    [string]$QmlDir = 'qt/qml'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$target = [IO.Path]::GetFullPath((Join-Path $repo $Out))
if (-not $target.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Deployment must stay inside this worktree.'
}
if (Test-Path -LiteralPath $target) { throw 'Use a fresh deployment directory; existing files are never removed.' }
$binary = [IO.Path]::GetFullPath((Join-Path $repo $Exe))
if (-not (Test-Path -LiteralPath $binary)) { throw "Missing executable: $binary" }
New-Item -ItemType Directory -Path $target | Out-Null
Copy-Item -LiteralPath $binary -Destination (Join-Path $target 'envhosts.exe')
& (Join-Path $QtDir 'bin/windeployqt.exe') --release --no-translations --qmldir (Join-Path $repo $QmlDir) --dir $target (Join-Path $target 'envhosts.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed: $LASTEXITCODE" }
if (-not $VcRedistDir -and $env:VCToolsRedistDir) {
    $VcRedistDir = (Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1).FullName
}
if (-not $VcRedistDir -or -not (Test-Path -LiteralPath (Join-Path $VcRedistDir 'vcruntime140.dll'))) {
    throw 'Provide the x64 VC runtime directory using -VcRedistDir or a VS developer shell.'
}
Get-ChildItem -LiteralPath $VcRedistDir -Filter '*.dll' | Copy-Item -Destination $target
$licenseTarget = Join-Path $target 'licenses/qt'
New-Item -ItemType Directory -Path $licenseTarget | Out-Null
$licenseSource = @((Join-Path $QtDir '../../Licenses'), (Join-Path $QtDir 'share/licenses'), (Join-Path $QtDir '../Licenses')) |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'LICENSE') } | Select-Object -First 1
if ($licenseSource) {
    Get-ChildItem -LiteralPath $licenseSource -File | Copy-Item -Destination $licenseTarget
} else {
    Copy-Item -LiteralPath (Join-Path $repo 'licenses/qt/LICENSE') -Destination $licenseTarget
}
Copy-Item -LiteralPath (Join-Path $repo 'licenses/qt/NOTICE.md') -Destination $licenseTarget
Write-Output $target
