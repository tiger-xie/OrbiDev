function Get-OrbiDevPackageVersion([string]$VersionFile) {
    $version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
    if ($version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'VERSION 不是有效的发布版本。' }
    return $version
}

function Assert-OrbiDevReleasePolicy([string]$Kind, [string]$Configuration, [string]$TestStatus, [bool]$Dirty) {
    if ($Kind -eq 'prerelease') {
        if ($Configuration -ne 'Release') { throw '公开预览包只允许 Release，不能分发 Debug 运行库。' }
        if ($TestStatus -ne 'Passed') { throw '公开预览包要求测试通过。' }
        if ($Dirty) { throw '公开预览包必须从干净的已提交工作区构建。' }
    }
}
