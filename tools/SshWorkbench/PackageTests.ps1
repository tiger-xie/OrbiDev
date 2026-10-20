$ErrorActionPreference = 'Stop'
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
Write-Host "打包验证：$passed 项通过；临时证据：$fixture"


