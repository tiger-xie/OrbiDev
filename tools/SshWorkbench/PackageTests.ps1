$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleasePolicy.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('orbidev-package-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$payload = Join-Path $fixture 'payload.txt'
Set-Content -LiteralPath $payload -Value 'fixture' -Encoding UTF8
$entry = @(@{ path = 'payload.txt'; sha256 = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash })
ConvertTo-Json -InputObject $entry | Set-Content -LiteralPath (Join-Path $fixture 'SHA256.json') -Encoding UTF8
& (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $fixture
$passed = 1
function Assert-Rejected([scriptblock]$Action) {
    $rejected = $false
    try { & $Action } catch { $rejected = $true }
    if (-not $rejected) { throw '应当拒绝但未拒绝。' }
}
Set-Content -LiteralPath $payload -Value 'changed' -Encoding UTF8
Assert-Rejected { & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $fixture }
$passed++
$entry[0].path = '../outside.txt'
ConvertTo-Json -InputObject $entry | Set-Content -LiteralPath (Join-Path $fixture 'SHA256.json') -Encoding UTF8
Assert-Rejected { & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $fixture }
$passed++
$entry[0].path = $payload
ConvertTo-Json -InputObject $entry | Set-Content -LiteralPath (Join-Path $fixture 'SHA256.json') -Encoding UTF8
Assert-Rejected { & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $fixture }
$passed++
$entry[0].path = 'payload.txt'
$entry[0].sha256 = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash
ConvertTo-Json -InputObject $entry | Set-Content -LiteralPath (Join-Path $fixture 'SHA256.json') -Encoding UTF8
Set-Content -LiteralPath (Join-Path $fixture 'unexpected.txt') -Value 'unexpected' -Encoding UTF8
Assert-Rejected { & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $fixture }
$passed++
# 同一未登记文件隐藏后也必须拒绝，不能因文件属性跳过完整性检查。
$unexpected = Get-Item -LiteralPath (Join-Path $fixture 'unexpected.txt')
$unexpected.Attributes = $unexpected.Attributes -bor [IO.FileAttributes]::Hidden
Assert-Rejected { & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $fixture }
$passed++
# 小型临时证据目录保留，避免测试清理误删路径。
Assert-OrbiDevReleasePolicy 'candidate' 'Debug' 'Failed' $true
Assert-OrbiDevReleasePolicy 'prerelease' 'Release' 'Passed' $false
$passed += 2
Assert-Rejected { Assert-OrbiDevReleasePolicy 'prerelease' 'Debug' 'Passed' $false }
Assert-Rejected { Assert-OrbiDevReleasePolicy 'prerelease' 'Release' 'Failed' $false }
Assert-Rejected { Assert-OrbiDevReleasePolicy 'prerelease' 'Release' 'NotRun' $false }
Assert-Rejected { Assert-OrbiDevReleasePolicy 'prerelease' 'Release' 'Passed' $true }
$passed += 4
$versionFile = Join-Path $fixture 'VERSION'
Set-Content -LiteralPath $versionFile -Value '0.1.0-preview.1' -Encoding ASCII
if ((Get-OrbiDevPackageVersion $versionFile) -ne '0.1.0-preview.1') { throw '版本读取错误。' }
Set-Content -LiteralPath $versionFile -Value '../invalid' -Encoding ASCII
Assert-Rejected { Get-OrbiDevPackageVersion $versionFile }
$passed += 2
# 无 BOM 中文 JSON 与 Windows PowerShell 5.1 的兼容回归。
$publicFixture = Join-Path $fixture 'public-utf8'
New-Item -ItemType Directory -Path $publicFixture | Out-Null
foreach ($required in @('LICENSE','THIRD_PARTY_NOTICES.md','licenses/LICENSE.LGPLv3','licenses/LICENSE.GPLv3','licenses/LICENSE.LGPLv21','licenses/LICENSE.Chromium','licenses/qt-5.15.2-notices/00001.txt','许可证样例.txt')) {
    $target = Join-Path $publicFixture $required
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    [IO.File]::WriteAllText($target, '中文许可夹具', (New-Object Text.UTF8Encoding($false)))
}
# 用标准库直接产生与 PowerShell 7 一致的 UTF8 无 BOM 格式，不要求额外安装 PowerShell 7。
function Write-FixtureJson([string]$Relative, $Value) {
    [IO.File]::WriteAllText((Join-Path $publicFixture $Relative), (ConvertTo-Json -InputObject $Value -Depth 4), (New-Object Text.UTF8Encoding($false)))
}
Write-FixtureJson 'manifest.json' @{ packageStatus='prerelease'; configuration='Release'; testStatus='Passed'; dirty=$false; sourceArchive='qt-everywhere-src-5.15.2.tar.xz'; note='公开预览验证' }
Write-FixtureJson 'source-manifest.json' @{ archive='qt-everywhere-src-5.15.2.tar.xz'; sha256='3a530d1b243b5dec00bc54937455471aaa3e56849d2593edb8ded07228202240'; licenseFileCount=1; note='许可集合保留源码相对路径；完整对应源码在同名附件中。' }
Write-FixtureJson 'licenses/qt-5.15.2-notices/index.json' @(@{ file='00001.txt'; sourcePath='qt-everywhere-src-5.15.2/许可夹具' })
$publicHashes = @(Get-ChildItem -LiteralPath $publicFixture -Recurse -File | ForEach-Object { @{ path=$_.FullName.Substring($publicFixture.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
Write-FixtureJson 'SHA256.json' $publicHashes
$sourceBytes = [IO.File]::ReadAllBytes((Join-Path $publicFixture 'source-manifest.json'))
if ($sourceBytes[0] -eq 0xEF -and $sourceBytes[1] -eq 0xBB) { throw '跨版本夹具必须不带 BOM。' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $publicFixture
if ($LASTEXITCODE -ne 0) { throw 'Windows PowerShell 5.1 无 BOM 中文包校验失败。' }
$passed++
# 哈希清单本身自洽也不能掩盖许可索引指向的文件缺失。
ConvertTo-Json -InputObject @(@{ file='00002.txt'; sourcePath='qt-everywhere-src-5.15.2/许可夹具' }) | Set-Content -LiteralPath (Join-Path $publicFixture 'licenses/qt-5.15.2-notices/index.json') -Encoding UTF8
$publicEntries = @(Get-ChildItem -LiteralPath $publicFixture -Recurse -File | Where-Object { $_.Name -ne 'SHA256.json' } | ForEach-Object { @{ path=$_.FullName.Substring($publicFixture.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
ConvertTo-Json -InputObject $publicEntries | Set-Content -LiteralPath (Join-Path $publicFixture 'SHA256.json') -Encoding UTF8
Assert-Rejected { & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -PackageDirectory $publicFixture }
$passed++
Write-Host "打包验证：$passed 项通过；临时证据：$fixture"


