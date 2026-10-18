$ErrorActionPreference = 'Stop'
$scriptBody = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Start-SshService.ps1') -Raw
$powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$cases = @(
    @{ Name = 'missing'; Service = '$null'; Answer = 'N'; Expected = '未安装 OpenSSH Server'; Code = 1 },
    @{ Name = 'running'; Service = "[pscustomobject]@{Status='Running'}"; Answer = 'N'; Expected = '已经运行'; Code = 0 },
    @{ Name = 'cancel'; Service = "[pscustomobject]@{Status='Stopped'}"; Answer = 'N'; Expected = '已取消'; Code = 0 },
    @{ Name = 'uac-cancel'; Service = "[pscustomobject]@{Status='Stopped'}"; Answer = 'Y'; Expected = '模拟取消 UAC'; Code = 1 }
)
foreach ($case in $cases) {
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString() + '.ps1')
    try {
        # 替换系统调用，确保测试不会启动服务或申请管理员权限。
        $mocks = @"
function Get-Service { param(`$Name, `$ErrorAction) return $($case.Service) }
function Read-Host { param(`$Prompt) return '$($case.Answer)' }
function Start-Service { throw '禁止真实启动服务' }
function Start-Process { throw '模拟取消 UAC' }
"@
        $body = $scriptBody -replace 'param\(\[switch\]\$Elevated\)', ('$Elevated = $false' + "`r`n" + $mocks)
        [IO.File]::WriteAllText($temporary, $body, [Text.UTF8Encoding]::new($true))
        $output = & $powershell -NoProfile -ExecutionPolicy Bypass -File $temporary 2>&1 | Out-String
        if ($LASTEXITCODE -ne $case.Code -or -not $output.Contains($case.Expected)) {
            throw "Failed: $($case.Name): $output"
        }
        Write-Host "Passed: $($case.Name)"
    } finally {
        Remove-Item -LiteralPath $temporary -Force
    }
}
