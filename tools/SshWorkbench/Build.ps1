param([string]$QtRoot = $env:QTDIR, [ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $QtRoot) { $QtRoot = 'D:/Qt/5.15.2/msvc2019_64' }
if (-not (Test-Path (Join-Path $QtRoot 'bin/windeployqt.exe'))) { throw 'Qt 5.15.2 was not found.' }
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source } else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ tools were not found.' }
    $cmakePath = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$buildDirectory = Join-Path $workspace 'build-ssh-workbench'
& $cmakePath -S $PSScriptRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 -T v143 "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmakePath --build $buildDirectory --config $Configuration --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$ctestPath = Join-Path (Split-Path $cmakePath) 'ctest.exe'
& $ctestPath --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
$packageDirectory = Join-Path $buildDirectory "package-$Configuration"
New-Item -ItemType Directory -Force -Path $packageDirectory | Out-Null
foreach ($script in @('Start-SshService.cmd', 'Start-SshService.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../SshSetup/$script") -Destination $packageDirectory -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../SshSetup/$script") -Destination (Join-Path $buildDirectory $Configuration) -Force
}
foreach ($application in @('OrbiDevWorkbench','OrbiDevSetup','OrbiDevHomeSetup')) {
    $executable = Join-Path $buildDirectory $(if ($application -eq 'OrbiDevWorkbench') { "$Configuration/$application.exe" } else { "setup/$Configuration/$application.exe" })
    & (Join-Path $QtRoot 'bin/windeployqt.exe') "--$($Configuration.ToLowerInvariant())" --no-translations $executable
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
    Copy-Item -LiteralPath $executable -Destination $packageDirectory -Force
    if ($application -ne 'OrbiDevWorkbench') {
        Copy-Item -LiteralPath $executable -Destination (Join-Path $buildDirectory $Configuration) -Force
    }
    & (Join-Path $QtRoot 'bin/windeployqt.exe') "--$($Configuration.ToLowerInvariant())" --no-translations (Join-Path $packageDirectory "$application.exe")
    if ($LASTEXITCODE -ne 0) { throw 'Package deployment failed.' }
}
Copy-Item -LiteralPath (Join-Path $workspace 'docs/SSH首次配置与家庭授权_使用说明.md') -Destination $packageDirectory -Force
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$runtime = Get-ChildItem -Path (Join-Path $installation 'VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT') -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $runtime) { throw 'MSVC x64 runtime was not found; portable package is incomplete.' }
Get-ChildItem -LiteralPath $runtime.FullName -Filter '*.dll' | Copy-Item -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $buildDirectory "$Configuration/conpty.dll") -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $buildDirectory "$Configuration/x64") -Destination $packageDirectory -Recurse -Force
Copy-Item -LiteralPath (Join-Path $workspace 'third_party/ConPTY-LICENSE.txt') -Destination $packageDirectory -Force
foreach ($license in @('xterm-LICENSE.txt', 'fit-LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $workspace "frontend/terminal/vendor/$license") -Destination $packageDirectory -Force
}
Copy-Item -LiteralPath (Join-Path $workspace 'docs/SSH内嵌工作台_使用与验证说明.md') -Destination $packageDirectory -Force
Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath (Join-Path $buildDirectory "OrbiDev-SshWorkbench-$Configuration.zip") -Force
Write-Host "Ready: $packageDirectory. Run OrbiDevWorkbench.exe on the client computer."
