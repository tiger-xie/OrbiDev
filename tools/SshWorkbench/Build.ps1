param([string]$QtRoot = $env:QTDIR, [ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$Candidate)
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
$testStatus = if ($LASTEXITCODE -eq 0) { 'Passed' } else { 'Failed' }
if ($testStatus -eq 'Failed' -and -not $Candidate) { throw '测试失败；停止打包。明确指定 -Candidate 才能生成标记失败的验证包。' }
# 保持仓库启动器可用；便携包仍从白名单重新部署。
$localDirectory = Join-Path $buildDirectory $Configuration
foreach ($application in @('OrbiDevSetup','OrbiDevHomeSetup')) {
    Copy-Item -LiteralPath (Join-Path $buildDirectory "setup/$Configuration/$application.exe") -Destination $localDirectory -Force
}
foreach ($script in @('Start-SshService.cmd','Start-SshService.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../SshSetup/$script") -Destination $localDirectory -Force
}
foreach ($application in @('OrbiDevWorkbench','OrbiDevSetup','OrbiDevHomeSetup')) {
    & (Join-Path $QtRoot 'bin/windeployqt.exe') "--$($Configuration.ToLowerInvariant())" --no-translations (Join-Path $localDirectory "$application.exe")
    if ($LASTEXITCODE -ne 0) { throw '本机构建目录 Qt 部署失败。' }
}
& (Join-Path $PSScriptRoot 'Package.ps1') -QtRoot $QtRoot -Configuration $Configuration -BuildDirectory $buildDirectory -TestStatus $testStatus
