param([switch]$Elevated)
$ErrorActionPreference = 'Stop'
try {
    $service = Get-Service -Name sshd -ErrorAction SilentlyContinue
    if (-not $service) {
        throw '未安装 OpenSSH Server。请在 Windows 设置 → 可选功能中安装 OpenSSH 服务器，再重新运行本脚本。若安装失败，请保留系统错误信息。'
    }
    if ($service.Status -eq 'Running') {
        Write-Host 'SSH 服务已经运行，无需重复启动。请返回工具点击“检查环境”。'
        exit 0
    }
    if (-not $Elevated) {
        Write-Host '即将执行：Start-Service sshd'
        Write-Host '这会启动 SSH 监听；首次启动可能由 OpenSSH 生成配置和主机密钥。'
        Write-Host '不会重启已有服务、设置开机启动、修改默认 Shell 或更改防火墙规则。访问范围沿用现有配置。'
        if ((Read-Host '确认启动？输入 Y 继续，其他输入取消') -ine 'Y') {
            Write-Host '已取消，未启动服务。'
            exit 0
        }
        $powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Elevated'
        $process = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw '启动未完成，请查看管理员窗口中的错误信息。' }
        Write-Host '操作完成，请返回家庭端工具点击“检查环境”。'
        exit 0
    }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '未取得管理员权限，未启动服务。'
    }
    Start-Service -Name sshd -ErrorAction Stop
    $service = Get-Service -Name sshd -ErrorAction Stop
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
    Write-Host 'SSH 服务已运行。请返回家庭端工具点击“检查环境”，继续检查配置与公钥授权。'
    Read-Host '按 Enter 关闭此窗口' | Out-Null
    exit 0
} catch {
    Write-Host ('操作失败：' + $_.Exception.Message) -ForegroundColor Red
    Write-Host '未自动安装组件或修复配置，请复制错误信息。'
    if ($Elevated) { Read-Host '按 Enter 关闭此窗口' | Out-Null }
    exit 1
}
