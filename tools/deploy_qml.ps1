#!/usr/bin/env pwsh
# deploy_qml.ps1 - assemble the self-contained Qt runtime for the QML editor build.
#
# Decision (see QML_MIGRATION_PHASE0.md §11): the plugin ships its own Qt 6
# runtime next to envhosts.exe. Win顺 exposes no third-party QML hosting API and
# does not promise its own Qt to plugins, so relying on the host is not an option.
#
# windeployqt cannot see the QML imports because the .qml files live in the
# plugin's qrc, so it is pointed at the source directory with --qmldir. That
# pulls every Qt Quick Controls style in; only Basic is used, so the rest is
# pruned afterwards. The result is ~45 MB.
#
# Usage:
#   pwsh tools/deploy_qml.ps1                       # build-qml/bin -> build-qml/deploy
#   pwsh tools/deploy_qml.ps1 -Out dist-runtime
#
# Then package with: python tools/package.py --runtime-dir build-qml/deploy

param(
    [string]$Exe = "build-qml/bin/envhosts.exe",
    [string]$Out = "build-qml/deploy",
    [string]$QtDir = "D:/tools/Qt/6.12.0/msvc2022_64",
    [string]$QmlDir = "qt/qml"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Exe)) { throw "missing $Exe; build with -DENVHOSTS_QML_EDITOR=ON first" }
$windeployqt = Join-Path $QtDir "bin/windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "missing $windeployqt; pass -QtDir" }

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Path $Out | Out-Null
Copy-Item $Exe (Join-Path $Out "envhosts.exe")

& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
    --qmldir $QmlDir (Join-Path $Out "envhosts.exe") | Out-Null

# Development-only plugin and obsolete formats; the editor shows text, not images.
foreach ($dir in @("qmltooling", "imageformats", "iconengines")) {
    Remove-Item (Join-Path $Out $dir) -Recurse -Force -ErrorAction SilentlyContinue
}

# Only the Basic style is used (QQuickStyle::setStyle("Basic") in qt_editor.cpp).
$styles = @("FluentWinUI3", "Imagine", "Material", "Universal", "Fusion", "Windows", "Native")
foreach ($style in $styles) {
    Remove-Item (Join-Path $Out "qml/QtQuick/Controls/$style") -Recurse -Force -ErrorAction SilentlyContinue
}
Get-ChildItem $Out -Filter "Qt6QuickControls2*.dll" -File |
    Where-Object { $_.Name -notmatch "Basic|Qt6QuickControls2\.dll|Qt6QuickControls2Impl\.dll" } |
    Remove-Item -Force

$unused = @(
    "Qt6QuickNativeStyle.dll", "Qt6Quick3DUtils.dll", "Qt6Lottie.dll",
    "Qt6LottieVectorImageGenerator.dll", "Qt6VirtualKeyboard.dll", "Qt6QuickEffects.dll",
    "Qt6QuickShapes.dll", "Qt6QuickVectorImage.dll", "Qt6QuickVectorImageGenerator.dll",
    "Qt6Svg.dll"
)
foreach ($name in $unused) {
    Remove-Item (Join-Path $Out $name) -Force -ErrorAction SilentlyContinue
}

# Qt is LGPLv3: the licence text must travel with the binaries.
$licenseDir = Join-Path (Split-Path -Parent $QtDir) "Licenses"
if (Test-Path $licenseDir) {
    New-Item -ItemType Directory -Path (Join-Path $Out "licenses/qt") -Force | Out-Null
    Copy-Item (Join-Path $licenseDir "LICENSE") (Join-Path $Out "licenses/qt/LICENSE") -Force
    Copy-Item (Join-Path $licenseDir "COPYING.txt") (Join-Path $Out "licenses/qt/COPYING.txt") -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $licenseDir "Copyright.txt") (Join-Path $Out "licenses/qt/Copyright.txt") -Force -ErrorAction SilentlyContinue
}

foreach ($required in @("envhosts.exe", "Qt6QuickControls2.dll", "Qt6QuickControls2Basic.dll",
                        "Qt6QuickControls2Impl.dll", "platforms/qwindows.dll", "qml/QtQuick/Controls/Basic")) {
    if (-not (Test-Path (Join-Path $Out $required))) { throw "deployment is missing $required" }
}

$bytes = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum
$count = (Get-ChildItem $Out -Recurse -File).Count
"deployed {0} files, {1:N1} MB -> {2}" -f $count, ($bytes / 1MB), $Out
