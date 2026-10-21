# OrbiDev 0.1.0-preview.1

首个 Windows x64 公开预览版本：在 Qt 桌面工作台内，通过 SSH / Tailscale 连接家庭 Windows 电脑运行 Claude Code。

## 下载

- 普通用户下载 `OrbiDev-0.1.0-preview.1-*.zip` 和对应 `.sha256` 校验文件。解压到新目录，运行 `Start-OrbiDev.cmd`。
- 家庭电脑首次授权运行 `Start-HomeSetup.cmd`；日常只需 sshd 和网络可用。
- `qt-everywhere-src-5.15.2.tar.xz` 是 Qt 对应源码附件，供许可查阅、重建或替换 Qt 使用，不是运行程序包。
- GitHub 自动生成的 Source code 是本项目源码；构建入口见 README。

运行包不附带 OpenSSH Server、Tailscale 或 Claude Code，请自行准备并确认使用资格。升级前保存远端工作并关闭旧版本，不要把新旧 DLL 混放。

## 本版功能

多主机和项目管理、内嵌多标签终端、首次设置与公钥授权、远程 Windows 目录选择和缓存、项目工作目录绑定、当前标签进程状态灯及 IPv4/IPv6 Ping 延迟。

## 验证与限制

干净源码基线 Release 构建及完整 CTest 14/14 通过。便携包的最终验证结果见随版本维护的 [验证记录](SSH工作台_公开预览版验证记录.md) 与包内清单。

这是 **Pre-release**，并未宣称完成长期稳定性验证：

- Debug Qt WebEngine 原生问题延期，候选 Qt 补丁没有应用。
- Release 历史上出现过偶发终端初始化超时，当前回归通过不代表根因已消除。
- Windows 11、全新 Windows 账户、长时间双端资源趋势及故障网络组合尚未全面验收。
- 关闭客户端、标签或网络断开，不保证远端任务继续；重连创建新会话。
- 绿灯只代表 SSH 进程运行；ICMP 不可测不等于 SSH 断开。

自有代码采用 MIT；Qt、Chromium、ConPTY 等保留各自许可证。运行包带许可说明，本次 Release 同时提供 Qt 对应源码与校验值。
