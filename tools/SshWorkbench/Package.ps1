param(
    [string]$QtRoot = $env:QTDIR,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateSet('Passed','Failed','NotRun')][string]$TestStatus = 'NotRun',
    [string]$BuildDirectory,
    [string]$OutputRoot,
    [string]$QtSourceRoot
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $QtRoot) { $QtRoot = 'D:/Qt/5.15.2/msvc2019_64' }
if (-not $QtSourceRoot) { $QtSourceRoot = Join-Path (Split-Path $QtRoot) 'Src' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $workspace 'build-ssh-workbench' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $BuildDirectory 'packages' }
$deployTool = Join-Path $QtRoot 'bin/windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployTool -PathType Leaf)) { throw '未找到 windeployqt.exe。' }
$revision = (& git -C $workspace rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw '无法取得 Git 提交。' }
$dirtyLines = @(& git -C $workspace status --porcelain)
if ($LASTEXITCODE -ne 0) { throw '无法取得 Git 工作区状态。' }
$version = '0.1.0-preview'
$kind = 'candidate'
$name = "OrbiDev-$version-$kind-$Configuration-$((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'))-$([Guid]::NewGuid().ToString('N').Substring(0,8))"
# 每次使用新目录，只拷贝白名单文件，避免旧包或本机配置残留。
$packageDirectory = Join-Path $OutputRoot $name
New-Item -ItemType Directory -Path $packageDirectory | Out-Null
foreach ($application in @('OrbiDevWorkbench','OrbiDevSetup','OrbiDevHomeSetup')) {
    $relative = if ($application -eq 'OrbiDevWorkbench') { "$Configuration/$application.exe" } else { "setup/$Configuration/$application.exe" }
    Copy-Item -LiteralPath (Join-Path $BuildDirectory $relative) -Destination $packageDirectory
    & $deployTool "--$($Configuration.ToLowerInvariant())" --no-translations (Join-Path $packageDirectory "$application.exe")
    if ($LASTEXITCODE -ne 0) { throw "Qt 部署失败：$application" }
}
foreach ($script in @('Start-SshService.cmd','Start-SshService.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../SshSetup/$script") -Destination $packageDirectory
}
foreach ($script in @('Start-OrbiDev.cmd','Start-HomeSetup.cmd')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "package/$script") -Destination $packageDirectory
}
Copy-Item -LiteralPath (Join-Path $BuildDirectory "$Configuration/conpty.dll") -Destination $packageDirectory
New-Item -ItemType Directory -Path (Join-Path $packageDirectory 'x64') | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDirectory "$Configuration/x64/OpenConsole.exe") -Destination (Join-Path $packageDirectory 'x64')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw '未找到 MSVC 运行库安装目录。' }
$runtime = Get-ChildItem -Path (Join-Path $installation 'VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT') -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $runtime) { throw '未找到 MSVC x64 运行库。' }
Get-ChildItem -LiteralPath $runtime.FullName -Filter '*.dll' | Copy-Item -Destination $packageDirectory
$licenseDirectory = Join-Path $packageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseDirectory | Out-Null
foreach ($source in @('third_party/ConPTY-LICENSE.txt','frontend/terminal/vendor/xterm-LICENSE.txt','frontend/terminal/vendor/fit-LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $workspace $source) -Destination $licenseDirectory
}
$missingLicenses = @()
foreach ($source in @('LICENSE.LGPLv3','LICENSE.LGPLv21','LICENSE.GPLv3','LICENSE.GPLv2','qtwebengine/LICENSE.Chromium')) {
    $path = Join-Path $QtSourceRoot $source
    if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination $licenseDirectory }
    else { $missingLicenses += $source }
}
if ($missingLicenses.Count) { throw 'Qt/Chromium 许可文本不完整，停止打包。' }
New-Item -ItemType Directory -Path (Join-Path $packageDirectory 'docs') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDirectory 'patches') | Out-Null
foreach ($source in @('SSH首次配置与家庭授权_使用说明.md','SSH内嵌工作台_使用与验证说明.md','SSH工作台_发布与升级说明.md','SSH配置工具_代码与验证说明.md','SSH-04_真实双端验收记录.md')) {
    Copy-Item -LiteralPath (Join-Path $workspace "docs/$source") -Destination (Join-Path $packageDirectory 'docs')
}
Copy-Item -LiteralPath (Join-Path $workspace 'patches/README.md') -Destination (Join-Path $packageDirectory 'patches')
@"
# OrbiDev $version — $kind

公司端运行 Start-OrbiDev.cmd；家庭电脑仅在首次授权时运行 Start-HomeSetup.cmd。
本包测试状态：$TestStatus。候选包仅用于验证，不代表全部发布验收通过。

- [发布、升级和验收边界](docs/SSH工作台_发布与升级说明.md)
- [日常使用](docs/SSH内嵌工作台_使用与验证说明.md)
- [首次配置与家庭授权](docs/SSH首次配置与家庭授权_使用说明.md)
- [已知 Qt 问题](patches/README.md)

关闭或断线不保证远程任务继续。许可文本位于 licenses；文件清单见 SHA256.json。
"@ | Set-Content -LiteralPath (Join-Path $packageDirectory 'README_使用说明.md') -Encoding UTF8
$suffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
$required = @('OrbiDevWorkbench.exe','OrbiDevSetup.exe','OrbiDevHomeSetup.exe',"QtWebEngineProcess$suffix.exe",'conpty.dll','x64/OpenConsole.exe',"platforms/qwindows$suffix.dll",'translations/qtwebengine_locales/en-US.pak','resources/icudtl.dat','resources/qtwebengine_resources.pak','resources/qtwebengine_resources_100p.pak','resources/qtwebengine_resources_200p.pak','resources/qtwebengine_devtools_resources.pak','vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')
foreach ($module in @('Core','Gui','Widgets','Network','PrintSupport','QmlModels','QuickWidgets','WebEngineCore','WebEngineWidgets','WebChannel','Qml','Quick','Positioning')) { $required += "Qt5$module$suffix.dll" }
foreach ($file in $required) {
    $path = Join-Path $packageDirectory $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) { throw "包缺少必需文件：$file" }
}
$manifest = [ordered]@{
    version = $version; gitHead = $revision; dirty = ($dirtyLines.Count -gt 0)
    configuration = $Configuration; packageStatus = $kind; testStatus = $TestStatus
    acceptanceComplete = $false; createdUtc = (Get-Date).ToUniversalTime().ToString('o')
    qtVersion = '5.15.2'; licenseTextsComplete = ($missingLicenses.Count -eq 0); missingLicenseTexts = $missingLicenses
    licenseReview = '文本随包提供；正式分发仍须核对开源许可和源码提供义务。'
    knownLimitations = @('SSH-KI-001：Qt WebEngine 原生竞态尚未修复。','关闭或断线不保证远程任务继续。','候选包不代表全部真实双端及 Windows 11 验收完成。')
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packageDirectory 'manifest.json') -Encoding UTF8
$hashes = @(Get-ChildItem -LiteralPath $packageDirectory -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($packageDirectory.Length + 1).Replace('\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
})
ConvertTo-Json -InputObject $hashes -Depth 3 | Set-Content -LiteralPath (Join-Path $packageDirectory 'SHA256.json') -Encoding UTF8
& (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $packageDirectory
$archive = "$packageDirectory.zip"
Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath $archive
(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash | Set-Content -LiteralPath "$archive.sha256" -Encoding ASCII
Write-Host "已生成 $kind 包：$packageDirectory"
Write-Output $packageDirectory



