param(
    [Parameter(Mandatory = $true)][ValidateSet('Agent', 'Client')][string]$Role,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$BuildDirectory = 'build-local',
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $repository $BuildDirectory
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$savedPath = $env:PATH
$savedToken = $env:RAD_AGENT_TOKEN
$savedPlugins = $env:QT_PLUGIN_PATH
$savedPlatformPlugins = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$savedWebEngine = $env:QTWEBENGINEPROCESS_PATH
$credential = $null

try {
    # 从 PowerShell 7 启动 Windows PowerShell 时，避免继承模块路径指向不兼容的安全模块。
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1')
    $application = 'Remote' + $Role
    $executable = Join-Path $BuildDirectory "apps/$application/$Configuration/$application.exe"
    $fakeCli = Join-Path $BuildDirectory "$Configuration/FakeCli.exe"
    foreach ($required in @($executable, $fakeCli)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "找不到 $required，请先构建 $Configuration；如只编译了 Release，请给脚本添加 -Configuration Release。"
        }
    }
    # 使用本次构建实际链接的 Qt，避免混用不同版本或 Debug/Release 运行库。
    $cache = Join-Path $BuildDirectory 'CMakeCache.txt'
    $qtEntry = @(Get-Content -LiteralPath $cache | Where-Object { $_ -match '^Qt5Core_DIR:PATH=' })
    if ($qtEntry.Count -ne 1) { throw '构建缓存中找不到 Qt5Core_DIR，请先完成 CMake 配置。' }
    $qtCmake = $qtEntry[0].Substring('Qt5Core_DIR:PATH='.Length)
    $qtRoot = [IO.Path]::GetFullPath((Join-Path $qtCmake '../../..'))
    $qtBin = Join-Path $qtRoot 'bin'
    $suffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
    if (-not (Test-Path -LiteralPath (Join-Path $qtBin "Qt5Core$suffix.dll"))) {
        throw "找不到当前构建对应的 Qt 运行库：$qtBin"
    }
    $env:PATH = "$qtBin;$savedPath"
    $env:QT_PLUGIN_PATH = Join-Path $qtRoot 'plugins'
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot 'plugins/platforms'
    $env:QTWEBENGINEPROCESS_PATH = Join-Path $qtBin "QtWebEngineProcess$suffix.exe"

    # 配置独立于构建类型，防止切换 Debug/Release 时误换设备身份。
    # 本目录被 Git 忽略；设备令牌由 Windows 当前用户加密，不保存明文。
    $localDirectory = Join-Path $repository '.local-run'
    $tokenFile = Join-Path $localDirectory 'device-token.dpapi'
    $configFile = Join-Path $localDirectory 'agent.json'
    $workspace = Join-Path $localDirectory 'workspace'
    if ($Role -eq 'Agent') {
        $listeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
        if (@($listeners | Where-Object { $_.Port -eq 17890 }).Count -gt 0) {
            throw '端口 17890 已被占用。如果 Agent 已启动，直接运行 Start-RemoteClient.cmd；本脚本不会结束已有进程。'
        }
        New-Item -ItemType Directory -Force -Path $workspace | Out-Null
        if (-not (Test-Path -LiteralPath $tokenFile)) {
            $bytes = New-Object byte[] 32
            $random = [Security.Cryptography.RandomNumberGenerator]::Create()
            try { $random.GetBytes($bytes) } finally { $random.Dispose() }
            $token = ([BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
            $secure = ConvertTo-SecureString $token -AsPlainText -Force
            $encrypted = ConvertFrom-SecureString $secure
            [IO.File]::WriteAllText($tokenFile, $encrypted)
        }
    }
    if (-not (Test-Path -LiteralPath $tokenFile)) {
        throw '本机联调配置尚未生成，请先双击 Start-RemoteAgent.cmd。'
    }
    $secure = ConvertTo-SecureString ([IO.File]::ReadAllText($tokenFile))
    $credential = New-Object System.Management.Automation.PSCredential('local-dev', $secure)
    $token = $credential.GetNetworkCredential().Password
    if ($token -cnotmatch '^[0-9a-f]{64}$') { throw '本机设备令牌无效，不能启动。' }

    if ($Role -eq 'Agent') {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $digest = $sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($token)) }
        finally { $sha.Dispose() }
        $config = [ordered]@{
            bind = '127.0.0.1'; port = 17890
            clients = @(@{ id = 'local-dev'; token_sha256 = ([BitConverter]::ToString($digest)).Replace('-', '').ToLowerInvariant() })
            workspaces = @(@{ id = 'demo'; path = $workspace; allowed_clients = @('local-dev') })
            fake_cli_path = $fakeCli
        }
        # Qt 的 JSON 解析使用无 BOM UTF-8；仅写脚本管理的专用配置。
        [IO.File]::WriteAllText($configFile, ($config | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    }
    elseif (-not $CheckOnly) {
        $probe = New-Object Net.Sockets.TcpClient
        try { $probe.Connect('127.0.0.1', 17890) }
        catch { throw '本机 Agent 尚未监听，请先启动 Start-RemoteAgent.cmd 并保留它的窗口。' }
        finally { $probe.Dispose() }
    }
    Write-Host "本机联调：$application / $Configuration / 127.0.0.1:17890 / 工作区 demo"
    if ($CheckOnly) {
        Write-Host '配置与程序路径检查通过，未启动应用。'
        exit 0
    }
    if ($Role -eq 'Agent') {
        $env:RAD_AGENT_TOKEN = $null
        Write-Host '请保留此窗口，再双击 Start-RemoteClient.cmd。停止 Agent 会结束其会话。'
        & $executable --config $configFile
    }
    else {
        # 令牌不进入命令行；Client 会在创建 WebEngine 前清除继承环境中的令牌。
        $env:RAD_AGENT_TOKEN = $token
        Write-Host '打开后点击“连接”，再点击“新建会话”；无需输入令牌。当前验证的是 FakeCli。'
        & $executable --agent-url 'ws://127.0.0.1:17890/' --workspace demo
    }
    if ($LASTEXITCODE -ne 0) { throw "$application 退出码：$LASTEXITCODE" }
}
catch {
    Write-Host ("启动失败：" + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    $env:PATH = $savedPath
    $env:RAD_AGENT_TOKEN = $savedToken
    $env:QT_PLUGIN_PATH = $savedPlugins
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPlatformPlugins
    $env:QTWEBENGINEPROCESS_PATH = $savedWebEngine
    $token = $null
    $credential = $null
}
