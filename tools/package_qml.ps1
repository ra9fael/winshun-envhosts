param(
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$VcRedistDir,
    [string]$BuildDir = 'build-qml',
    [string]$OutputDir = 'dist/qml-release',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
if (Test-Path -LiteralPath $output) { throw 'Use a fresh output directory.' }
& (Join-Path $PSScriptRoot 'deploy_qml.ps1') -Exe "$BuildDir/bin/envhosts.exe" -Out "$OutputDir/runtime" -QtDir $QtRoot -VcRedistDir $VcRedistDir
& $Python (Join-Path $PSScriptRoot 'package.py') --runtime-dir (Join-Path $output 'runtime') --out $output
if ($LASTEXITCODE -ne 0) { throw "Package validation failed: $LASTEXITCODE" }
