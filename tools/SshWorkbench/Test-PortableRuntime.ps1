[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$packagePath = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\', '/')
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$qtPath = (Resolve-Path -LiteralPath $QtRoot).Path
$outputPath = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if ($outputPath.Equals($packagePath, [StringComparison]::OrdinalIgnoreCase) -or
    $outputPath.StartsWith($packagePath + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw '验证输出目录不能位于原发布包内。'
}
if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'WorkbenchTerminalTests.exe'))) {
    $buildPath = Join-Path $buildPath 'Release'
}
$testNames = @('WorkbenchTerminalTests', 'SshSessionTests')
$testFiles = @('WorkbenchTerminalTests.exe', 'SshSessionTests.exe', 'FakeCli.exe')
foreach ($file in $testFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildPath $file) -PathType Leaf)) {
        throw "缺少同一 Release 构建的测试程序：$file"
    }
    if (Test-Path -LiteralPath (Join-Path $packagePath $file)) {
        throw "原发布包含测试程序，需先检查打包内容：$file"
    }
}
foreach ($file in @('OrbiDevWorkbench.exe', 'Qt5Core.dll', 'Qt5WebEngineCore.dll',
                    'QtWebEngineProcess.exe', 'conpty.dll', 'x64/OpenConsole.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $packagePath $file) -PathType Leaf)) {
        throw "发布包缺少运行文件：$file"
    }
}
$qtTest = Join-Path $qtPath 'bin/Qt5Test.dll'
if (-not (Test-Path -LiteralPath $qtTest -PathType Leaf)) {
    throw 'QtRoot 中没有 Release Qt5Test.dll。'
}
if ((Get-FileHash -LiteralPath (Join-Path $packagePath 'OrbiDevWorkbench.exe')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $buildPath 'OrbiDevWorkbench.exe')).Hash) {
    throw '发布包主程序与指定构建不一致，不能混用另一构建的测试程序。'
}
foreach ($file in @('Qt5Core.dll', 'Qt5WebEngineCore.dll')) {
    $packagedHash = (Get-FileHash -LiteralPath (Join-Path $packagePath $file)).Hash
    $sdkHash = (Get-FileHash -LiteralPath (Join-Path $qtPath "bin/$file")).Hash
    if ($packagedHash -ne $sdkHash) {
        throw "发布包与测试 Qt SDK 不匹配：$file"
    }
}

# 保存原包文件清单与哈希；所有测试附加文件仅进入独立副本。
$packageFiles = @(Get-ChildItem -LiteralPath $packagePath -Recurse -Force)
if (@($packageFiles | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
    throw '发布包包含重解析点，不能保证独立复制，请先检查。'
}
$originalHashes = @{}
foreach ($file in $packageFiles | Where-Object { -not $_.PSIsContainer }) {
    $originalHashes[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName).Hash
}
$runPath = Join-Path $outputPath ('portable-runtime-' + [Guid]::NewGuid().ToString('N'))
$runtimePath = Join-Path $runPath 'runtime'
New-Item -ItemType Directory -Path $runtimePath -Force | Out-Null
Get-ChildItem -LiteralPath $packagePath -Force | Copy-Item -Destination $runtimePath -Recurse -Force
foreach ($file in $testFiles) {
    Copy-Item -LiteralPath (Join-Path $buildPath $file) -Destination $runtimePath
}
Copy-Item -LiteralPath $qtTest -Destination $runtimePath

$results = @()
$abort = $false
$summaryPath = Join-Path $runPath 'results.json'
# 每轮使用新进程；失败也保留原始结果，不以复跑覆盖首轮。
foreach ($round in 1..3) {
    foreach ($testName in $testNames) {
        $prefix = Join-Path $runPath ("round-$round-$testName")
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = Join-Path $runtimePath "$testName.exe"
        $start.WorkingDirectory = $runtimePath
        $start.Arguments = '-o "' + $prefix + '.txt,txt"'
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        # 子进程不继承开发 Qt 路径、插件路径、缩放或 Chromium 调试开关。
        foreach ($key in @($start.EnvironmentVariables.Keys)) {
            if ($key -match '^(QT|QML|QTDIR|QMAKESPEC|ORBIDEV_)') {
                $start.EnvironmentVariables.Remove($key)
            }
        }
        $start.EnvironmentVariables['PATH'] = @(
            "$env:SystemRoot\System32", $env:SystemRoot,
            "$env:SystemRoot\System32\Wbem",
            "$env:SystemRoot\System32\WindowsPowerShell\v1.0",
            "$env:SystemRoot\System32\OpenSSH"
        ) -join ';'
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $start
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $exitCode = $null
        $timedOut = $false
        $failure = $null
        try {
            if (-not $process.Start()) { throw '测试进程启动失败。' }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(180000)) {
                $timedOut = $true
                $abort = $true
                # 只终止本脚本刚创建的测试进程，绝不按名称终止用户工作台。
                $process.Kill()
                $process.WaitForExit()
            }
            $exitCode = $process.ExitCode
            if ($stdout.Wait(5000) -and $stderr.Wait(5000)) {
                $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath "$prefix.stdout.txt" -Encoding UTF8
                $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath "$prefix.stderr.txt" -Encoding UTF8
            }
            else {
                $failure = '测试进程已退出，但输出管道未关闭，停止后续轮次。'
                $abort = $true
            }
        }
        catch {
            $failure = $_.Exception.Message
            $abort = $true
        }
        finally {
            $watch.Stop()
            $process.Dispose()
        }
        $results += [pscustomobject]@{
            round = $round; suite = $testName; exitCode = $exitCode
            timedOut = $timedOut; elapsedMs = $watch.ElapsedMilliseconds
            failure = $failure; log = "$prefix.txt"
        }
        $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
        Write-Output "$testName round=$round exit=$exitCode timeout=$timedOut elapsedMs=$($watch.ElapsedMilliseconds)"
        if ($abort) { break }
    }
    if ($abort) { break }
}

$packageUnchanged = $true
$currentFiles = @(Get-ChildItem -LiteralPath $packagePath -Recurse -Force -File)
if ($currentFiles.Count -ne $originalHashes.Count) { $packageUnchanged = $false }
foreach ($file in $currentFiles) {
    if (-not $originalHashes.ContainsKey($file.FullName) -or
        $originalHashes[$file.FullName] -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
        $packageUnchanged = $false
    }
}
[pscustomobject]@{
    packageUnchanged = $packageUnchanged
    roundsCompleted = $results.Count
    expectedSuiteRuns = 6
    runtimeCopy = $runtimePath
    note = '副本和日志保留供检查；未连接真实主机，未启动真实 Claude。超时后停止后续轮次，不按名称清理其他进程。'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runPath 'verification.json') -Encoding UTF8
Write-Output "验证结果：$summaryPath"
if (-not $packageUnchanged -or $results.Count -ne 6 -or
    @($results | Where-Object { $_.exitCode -ne 0 -or $_.timedOut -or $_.failure }).Count) {
    exit 1
}
exit 0
