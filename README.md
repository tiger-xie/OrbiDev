# OrbiDev · SSH 工作台

一个 Windows Qt 桌面工作台，通过 SSH / Tailscale 连接自己的家庭电脑，在多个项目标签中使用远端 Claude Code。

**当前为预览版。** 关闭标签、退出客户端或网络断开，不保证远端任务继续运行；重新连接会创建新会话。没有远端任务托管或会话恢复承诺。

## 功能

- 多主机、多项目、多标签终端；双击复用已有会话，显式新建额外终端。
- 首次配置向导、专用 SSH 身份、家庭端公钥授权及连接诊断。
- 只读浏览 Windows 家庭端目录，当前弹窗缓存已访问目录；选定目录作为新 Claude 会话的工作目录。
- 状态灯、当前标签目标主机的 ICMP 延迟、失败原因及重新连接。
- 中文输入、复制粘贴、终端字号和窗口布局保存。

终端链路：Qt TerminalView → 本机 ConPTY → Windows OpenSSH → 家庭端 PowerShell → Claude Code。主机指纹和 Claude 工作区信任由用户核对，软件不会自动接受。

## 下载与运行

在 [Releases](https://github.com/tiger-xie/OrbiDev/releases) 页面下载 Windows x64 ZIP，解压到新目录，运行 `Start-OrbiDev.cmd`。预览版本请查看 **Pre-release** 条目和其已知问题。

家庭电脑首次使用运行 `Start-HomeSetup.cmd` 导入公司电脑的 SSH 公钥。日常连接不要求家庭端配置工具常驻，但 sshd 与网络连接必须可用。

包中不包含 Tailscale、OpenSSH Server 或 Claude Code。请自行安装、登录并确认其使用资格。不要把 SSH 私钥、访问令牌或真实终端内容发到 Issues。

- [首次配置与家庭授权](docs/SSH首次配置与家庭授权_使用说明.md)
- [日常使用与目录/状态说明](docs/SSH内嵌工作台_使用与验证说明.md)
- [升级、回退与验证边界](docs/SSH工作台_发布与升级说明.md)
- [版本记录](CHANGELOG.md)

## 从源码构建

需要 Windows x64、Visual Studio 2022 的 C++ 桌面开发工具（v143）、CMake 3.21+，以及 **Qt 5.15.2 MSVC 2019 x64**（包含 WebEngine、WebChannel、Qt Test）。项目使用 C++20。使用 Qt 开源组件须遵守其许可证。

在 PowerShell 中运行（按实际安装位置修改 Qt 路径）：

```powershell
$env:QTDIR = 'D:/Qt/5.15.2/msvc2019_64'
cmake -S tools/SshWorkbench -B build-ssh-workbench -G 'Visual Studio 17 2022' -A x64 -T v143 "-DCMAKE_PREFIX_PATH=$env:QTDIR"
cmake --build build-ssh-workbench --config Release --parallel 4
ctest --test-dir build-ssh-workbench -C Release --output-on-failure
```

ConPTY 从官方 NuGet 下载固定版本并校验 SHA-256；离线构建可通过 `-DRAD_CONPTY_ARCHIVE=包的绝对路径` 指定同一版本文件。构建需要网络或预先准备依赖。

`Build-SshWorkbench.cmd` 是本地构建、测试和候选打包入口，也可运行 `tools/SshWorkbench/Build.ps1 -QtRoot <Qt路径>`。测试失败默认停止打包；不要把候选失败包当作稳定发行版。普通开发启动使用 `Start-OrbiDev.cmd`。

根目录 CMake 及 `apps/RemoteAgent` / `core` 中另有暂停的 Client/Agent 原型。它们不是本版工作台运行所需的服务；当前开发入口是 `tools/SshWorkbench`。

## 验证范围与限制

- 主要本机验证环境为 Windows 10 x64；Windows 11 和全新账户环境尚未完成全面验收。
- Qt WebEngine 存在已记录的 Debug 原生问题，Release 曾出现偶发终端初始化超时。单次回归通过不代表风险消除，详见 [已知问题](patches/README.md)。
- Ping 不通可能是 ICMP 被禁用，不等于 SSH 断开；绿灯仅表示 SSH 进程运行，不代表 Claude 已完成认证或开始工作。
- 远端目录第一版只支持 Windows，只浏览和选择目录，不提供文件上传、下载或删除。
- 需要连接可信的自有主机；项目不提供绕过地区限制、服务审查或账号授权的能力。

## 开源与贡献

项目自有代码使用 [MIT](LICENSE)。第三方依赖和图标保留各自许可证，详见 [第三方清单](THIRD_PARTY_NOTICES.md)。Qt 动态库不受本项目 MIT 许可证重新授权。

欢迎提交问题和 Pull Request。复现信息请包含版本、Windows 版本、操作步骤和脱敏错误；安全问题请按 [SECURITY.md](SECURITY.md) 处理。贡献约定见 [CONTRIBUTING.md](CONTRIBUTING.md)。
