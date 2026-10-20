param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$TargetProcessId,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 1800,
    [ValidateRange(1, 60)]
    [int]$IntervalSeconds = 10
)

$ErrorActionPreference = 'Stop'
$target = Get-Process -Id $TargetProcessId
if ($target.ProcessName -ne 'OrbiDevWorkbench') {
    throw '目标必须是 OrbiDevWorkbench 进程。'
}
$startedAt = $target.StartTime.ToUniversalTime()
$outputFile = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputFile) {
    throw '输出文件已存在，请指定新文件以保留先前证据。'
}
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($outputFile)) -PathType Container)) {
    throw '输出目录不存在，请先创建目录。'
}

# 仅记录主进程资源，不读取配置、终端内容或命令行；不操控被测进程。
$watch = [Diagnostics.Stopwatch]::StartNew()
$samples = 0
$reason = 'duration-complete'
while ($true) {
    $target = Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
    if (-not $target) {
        $reason = 'process-exited'
        break
    }
    if ($target.StartTime.ToUniversalTime() -ne $startedAt) {
        $reason = 'process-id-reused'
        break
    }
    [pscustomobject]@{
        TimeUtc = [DateTime]::UtcNow.ToString('o')
        ElapsedSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 2)
        ProcessId = $TargetProcessId
        ProcessStartUtc = $startedAt.ToString('o')
        PrivateBytes = $target.PrivateMemorySize64
        WorkingSetBytes = $target.WorkingSet64
        Handles = $target.HandleCount
        Threads = $target.Threads.Count
    } | Export-Csv -LiteralPath $outputFile -NoTypeInformation -Encoding UTF8 -Append
    ++$samples
    if ($watch.Elapsed.TotalSeconds -ge $DurationSeconds) {
        break
    }
    $remaining = $DurationSeconds - $watch.Elapsed.TotalSeconds
    Start-Sleep -Milliseconds ([int][Math]::Max(1, [Math]::Min($IntervalSeconds, $remaining) * 1000))
}
Write-Output "reason=$reason samples=$samples elapsedSeconds=$([Math]::Round($watch.Elapsed.TotalSeconds, 2))"
Write-Output "csv=$outputFile"
if ($reason -ne 'duration-complete') {
    # 进程提前退出不能记作完整时长验收通过。
    exit 2
}
