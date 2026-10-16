param(
    [string]$BuildDirectory = 'build-local',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$QtRoot = $env:QTDIR,
    [string]$Destination = 'build-m1-deploy'
)

$ErrorActionPreference = 'Stop'
if (-not $QtRoot) { throw '请设置 QTDIR 或通过 -QtRoot 指定 Qt 5.15.2 目录。' }
$deployer = Join-Path $QtRoot 'bin/windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployer)) { throw "找不到部署工具：$deployer" }
$configurationFlag = '--' + $Configuration.ToLowerInvariant()

# 本脚本只复制到运行目录，不清理已有内容，也不安装或修改系统运行库。
foreach ($application in @('RemoteClient', 'RemoteAgent')) {
    $source = Join-Path $BuildDirectory "apps/$application/$Configuration/$application.exe"
    if (-not (Test-Path -LiteralPath $source)) { throw "请先构建：$source" }
    $folder = Join-Path $Destination "$Configuration/$application"
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $executable = Join-Path $folder "$application.exe"
    Copy-Item -LiteralPath $source -Destination $executable -Force
    & $deployer $configurationFlag --no-compiler-runtime --dir $folder $executable
    if ($LASTEXITCODE -ne 0) { throw "$application 部署失败：$LASTEXITCODE" }
    Set-Content -LiteralPath (Join-Path $folder 'qt.conf') -Encoding UTF8 -Value "[Paths]`nPrefix=."
}

$savedPath = $env:PATH
$savedPluginPath = $env:QT_PLUGIN_PATH
try {
    # 验证运行目录自身的依赖，避免开发 Qt 的 PATH 或插件路径掩盖缺失文件。
    $env:PATH = "$env:SystemRoot/system32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = ''
    foreach ($application in @('RemoteClient', 'RemoteAgent')) {
        $executable = Join-Path $Destination "$Configuration/$application/$application.exe"
        & (Resolve-Path -LiteralPath $executable).Path --smoke-test
        if ($LASTEXITCODE -ne 0) { throw "$application 部署烟测失败：$LASTEXITCODE" }
        Write-Output "$application 部署烟测通过。"
    }
}
finally {
    $env:PATH = $savedPath
    $env:QT_PLUGIN_PATH = $savedPluginPath
}
