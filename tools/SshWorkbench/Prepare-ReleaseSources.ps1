param([string]$OutputDirectory, [switch]$DownloadOnly)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $workspace 'build-ssh-workbench/release-sources' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$name = 'qt-everywhere-src-5.15.2.tar.xz'
$url = "https://download.qt.io/archive/qt/5.15/5.15.2/single/$name"
# 固定值来自 Qt 官方 mirrorlist；下载的官方校验文本也必须与它一致。
$expectedHash = '3a530d1b243b5dec00bc54937455471aaa3e56849d2593edb8ded07228202240'
$archive = Join-Path $OutputDirectory $name
$checksum = Join-Path $OutputDirectory "$name.sha256"
& curl.exe -fLsS --retry 3 --connect-timeout 30 --max-time 120 -o $checksum "$url.sha256"
if ($LASTEXITCODE -ne 0) { throw '获取 Qt 官方 SHA256 失败。' }
$official = Get-Content -LiteralPath $checksum -Raw
if ($official -notmatch "(?i)\b$expectedHash\b") { throw 'Qt 官方 SHA256 与固定版本记录不一致。' }
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    $partial = "$archive.partial"
    & curl.exe -fL --retry 3 --connect-timeout 30 --continue-at - -o $partial $url
    if ($LASTEXITCODE -ne 0) { throw 'Qt 对应源码下载未完成；保留断点文件。' }
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Qt 源码 SHA256 不匹配。' }
    Move-Item -LiteralPath $partial -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) { throw '已有 Qt 源码 SHA256 不匹配。' }
if ($DownloadOnly) { Write-Output $archive; return }
# 只提取许可、版权和归属说明，完整源码作为独立附件保留，不展开数 GB 源文件。
$collection = Join-Path $OutputDirectory 'qt-5.15.2-notices'
New-Item -ItemType Directory -Force -Path $collection | Out-Null
# 旧版 Windows tar 不支持 xz；使用 Python 3 标准库，不增加产品运行依赖。
$extractionJson = & python.exe (Join-Path $PSScriptRoot 'Extract-QtLicenses.py') $archive $collection
if ($LASTEXITCODE -ne 0) { throw '提取 Qt 许可集合失败；需要带 lzma 的 Python 3。' }
$extraction = $extractionJson | ConvertFrom-Json
$manifest = [ordered]@{
    qtVersion = '5.15.2'; archive = $name; sha256 = $expectedHash
    sourceUrl = $url; checksumUrl = "$url.sha256"; modifications = 'none'
    licenseCollection = 'qt-5.15.2-notices'; licenseFileCount = $extraction.count
    note = '许可集合保留源码相对路径；包含构建未使用的组件，不表示全部组件均被链接。完整对应源码在同名附件中。'
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'source-manifest.json') -Encoding UTF8
Write-Output $OutputDirectory
