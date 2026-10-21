param(
    [string]$QtRoot = $env:QTDIR,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateSet('Passed','Failed','NotRun')][string]$TestStatus = 'NotRun',
    [string]$BuildDirectory,
    [string]$OutputRoot,
    [string]$QtSourceRoot,
    [ValidateSet('candidate','prerelease')][string]$Kind = 'candidate',
    [string]$ReleaseSourcesDirectory
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $PSScriptRoot 'ReleasePolicy.ps1')
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
$version = Get-OrbiDevPackageVersion (Join-Path $workspace 'VERSION')
Assert-OrbiDevReleasePolicy $Kind $Configuration $TestStatus ($dirtyLines.Count -gt 0)
$sourceManifest = $null
if ($Kind -eq 'prerelease') {
    if (-not $ReleaseSourcesDirectory) { throw '公开预览包必须指定 Prepare-ReleaseSources.ps1 生成的 ReleaseSourcesDirectory。' }
    $ReleaseSourcesDirectory = (Resolve-Path -LiteralPath $ReleaseSourcesDirectory).Path
    $sourceManifest = Get-Content -LiteralPath (Join-Path $ReleaseSourcesDirectory 'source-manifest.json') -Raw | ConvertFrom-Json
    $expectedQtHash = '3a530d1b243b5dec00bc54937455471aaa3e56849d2593edb8ded07228202240'
    if ($sourceManifest.archive -ne 'qt-everywhere-src-5.15.2.tar.xz' -or $sourceManifest.sha256 -ne $expectedQtHash -or $sourceManifest.modifications -ne 'none' -or $sourceManifest.licenseCollection -ne 'qt-5.15.2-notices') { throw 'Qt 源码清单与本发布版本不匹配。' }
    if ((Get-FileHash -LiteralPath (Join-Path $ReleaseSourcesDirectory $sourceManifest.archive) -Algorithm SHA256).Hash -ne $expectedQtHash) { throw 'Qt 对应源码校验失败。' }
    if (-not (Test-Path -LiteralPath (Join-Path $ReleaseSourcesDirectory 'qt-5.15.2-notices/index.json'))) { throw 'Qt 许可集合不完整。' }
}
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
Copy-Item -LiteralPath (Join-Path $workspace 'LICENSE') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $workspace 'THIRD_PARTY_NOTICES.md') -Destination $packageDirectory
if ($sourceManifest) {
    Copy-Item -LiteralPath (Join-Path $ReleaseSourcesDirectory 'qt-5.15.2-notices') -Destination $licenseDirectory -Recurse
    Copy-Item -LiteralPath (Join-Path $ReleaseSourcesDirectory 'source-manifest.json') -Destination $packageDirectory
}
foreach ($source in @('third_party/ConPTY-LICENSE.txt','frontend/terminal/vendor/xterm-LICENSE.txt','frontend/terminal/vendor/fit-LICENSE.txt','resources/icon/workbench/LICENSE-Lucide.txt')) {
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
foreach ($source in @('SSH工作台_公开预览版验证记录.md','SSH工作台_首个公开版本说明.md')) {
    $document = Join-Path $workspace "docs/$source"
    if ($Kind -eq 'prerelease' -or (Test-Path -LiteralPath $document)) {
        Copy-Item -LiteralPath $document -Destination (Join-Path $packageDirectory 'docs')
    }
}
Copy-Item -LiteralPath (Join-Path $workspace 'patches/README.md') -Destination (Join-Path $packageDirectory 'patches')
@"
# OrbiDev $version — $kind

公司端运行 Start-OrbiDev.cmd；家庭电脑仅在首次授权时运行 Start-HomeSetup.cmd。
本包测试状态：$TestStatus。$Kind 包仍有公开的已知风险，不代表全部真实环境验收通过。

- [发布、升级和验收边界](docs/SSH工作台_发布与升级说明.md)
- [当前公开预览版验证记录](docs/SSH工作台_公开预览版验证记录.md)
- [首个公开版本说明](docs/SSH工作台_首个公开版本说明.md)
- [日常使用](docs/SSH内嵌工作台_使用与验证说明.md)
- [首次配置与家庭授权](docs/SSH首次配置与家庭授权_使用说明.md)
- [已知 Qt 问题](patches/README.md)

关闭或断线不保证远程任务继续。自有代码许可见 LICENSE；第三方许可、Qt 对应源码及替换说明见 THIRD_PARTY_NOTICES.md 和 licenses；文件清单见 SHA256.json。
"@ | Set-Content -LiteralPath (Join-Path $packageDirectory 'README_使用说明.md') -Encoding UTF8
$suffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
$required = @('OrbiDevWorkbench.exe','OrbiDevSetup.exe','OrbiDevHomeSetup.exe',"QtWebEngineProcess$suffix.exe",'conpty.dll','x64/OpenConsole.exe',"platforms/qwindows$suffix.dll",'translations/qtwebengine_locales/en-US.pak','resources/icudtl.dat','resources/qtwebengine_resources.pak','resources/qtwebengine_resources_100p.pak','resources/qtwebengine_resources_200p.pak','resources/qtwebengine_devtools_resources.pak','vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')
foreach ($module in @('Core','Gui','Widgets','Network','PrintSupport','QmlModels','QuickWidgets','WebEngineCore','WebEngineWidgets','WebChannel','Qml','Quick','Positioning','Svg')) { $required += "Qt5$module$suffix.dll" }
$required += "iconengines/qsvgicon$suffix.dll"
$required += "imageformats/qsvg$suffix.dll"
foreach ($file in $required) {
    $path = Join-Path $packageDirectory $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) { throw "包缺少必需文件：$file" }
}
$manifest = [ordered]@{
    version = $version; gitHead = $revision; dirty = ($dirtyLines.Count -gt 0)
    configuration = $Configuration; packageStatus = $kind; testStatus = $TestStatus
    acceptanceComplete = $false; createdUtc = (Get-Date).ToUniversalTime().ToString('o')
    qtVersion = '5.15.2'; licenseTextsComplete = ($missingLicenses.Count -eq 0); missingLicenseTexts = $missingLicenses
    licenseReview = $(if ($sourceManifest) { '附项目 MIT、第三方许可集合及匹配 Qt 源码清单；发布时必须同时上传源码附件。' } else { '候选包仅有基本许可文本，不可直接作为公开分发包。' })
    sourceArchive = $(if ($sourceManifest) { $sourceManifest.archive } else { $null })
    knownLimitations = @('SSH-KI-001：Qt WebEngine 原生竞态尚未修复。','Release 偶发终端初始化超时的根因尚未确认；本轮测试通过不代表该问题已修复。','关闭或断线不保证远程任务继续。','候选包不代表全部真实双端及 Windows 11 验收完成。')
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
if ($sourceManifest) {
    # 此清单供发布工具读取，不能只上传二进制 ZIP 而漏掉对应源码。
    $assets = @($archive, "$archive.sha256", (Join-Path $ReleaseSourcesDirectory $sourceManifest.archive), (Join-Path $ReleaseSourcesDirectory ($sourceManifest.archive + '.sha256')), (Join-Path $ReleaseSourcesDirectory 'source-manifest.json'))
    ConvertTo-Json -InputObject $assets | Set-Content -LiteralPath "$archive.release-assets.json" -Encoding UTF8
}
Write-Host "已生成 $kind 包：$packageDirectory"
Write-Output $packageDirectory



