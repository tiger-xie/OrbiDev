param([Parameter(Mandatory = $true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\','/')
# 不跟随重解析点，避免校验访问包外文件。
$pending = New-Object 'System.Collections.Generic.Queue[string]'
$pending.Enqueue($root)
while ($pending.Count -gt 0) {
    $directory = $pending.Dequeue()
    if (((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw '包目录包含重解析点。' }
    foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw '包内不允许重解析点。' }
        if ($item.PSIsContainer) { $pending.Enqueue($item.FullName) }
    }
}
$entries = @(Get-Content -LiteralPath (Join-Path $root 'SHA256.json') -Raw | ConvertFrom-Json)
if (-not $entries.Count) { throw '文件校验列表为空。' }
$seen = @{}
foreach ($entry in $entries) {
    $relative = [string]$entry.path
    if ($relative -match ':' -or [IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') { throw '校验列表包含非法路径。' }
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw '文件超出包目录。' }
    if ($seen.ContainsKey($relative)) { throw "重复校验项：$relative" }
    $seen[$relative] = $true
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "文件缺失：$relative" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "文件校验失败：$relative" }
}
foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse) {
    $relative = $file.FullName.Substring($root.Length + 1).Replace('\','/')
    if ($relative -ne 'SHA256.json' -and -not $seen.ContainsKey($relative)) { throw "发现未登记文件：$relative" }
}
Write-Host "文件校验通过：$($entries.Count) 项。"

