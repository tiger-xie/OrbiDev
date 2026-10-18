# OrbiDev 内嵌 SSH 工作台

本轮在 `codex/ssh-setup-wizards` 分支继续开发。首次配置与家庭授权基线提交为 `2393728`；内嵌工作台为后续工作区实现。旧 Client/Agent 和外部 Windows Terminal 启动器保留。

## 启动与使用

1. 在公司电脑完成首次配置，连接测试通过后点击“保存配置并完成”。
2. 双击仓库根目录 `Build-SshWorkbench.cmd` 构建，完成后双击 `Start-SshWorkbench.cmd`。也可解压工作台携带包运行 `OrbiDevWorkbench.exe`。
3. 左侧添加项目，填写家庭电脑上的目录；留空使用家庭用户目录。选中项目点击“新建终端”，或勾选多个项目点击“打开勾选项目”。
4. 每个标签中直接显示 SSH 和 Claude。工作区信任提示由用户自己确认。状态“SSH 运行中”只说明本地进程存在，不代表 Claude 已就绪。
5. 复制／粘贴使用 `Ctrl+Shift+C`／`Ctrl+Shift+V`；缩放使用工具栏字号按钮或 `Ctrl+=`／`Ctrl+-`。中文、Tab、方向键等交给内嵌终端与 SSH 传递。
6. 关闭活动标签或整个窗口需要确认；断开后不保证远端任务继续。重新打开项目会创建新会话。

当前实现为一台已配置主机上的多项目、多标签工作台，不包含多主机档案管理。连接设置由首次配置工具维护。打开中的标签保留创建时的主机信息；更新连接设置只影响新标签。

## 配置兼容

工作台读取首次配置工具的 `connection.json`。项目单独保存在当前用户 `OrbiDev/OrbiDevWorkbench/projects.json`。根目录启动脚本通过 `--legacy-config` 只读导入 `.local-run/launcher-projects.json`；编辑或勾选后写入新的项目文件，保留旧名称、目录、勾选状态及兼容字段 `terminal_path`。内嵌模式不要求安装 Windows Terminal。

命令行支持 `--config`、`--projects`、`--legacy-config` 指定配置位置。`--screenshot` 只渲染预览，不读写用户配置或连接 SSH。

## 源码导航

路径相对仓库根目录。

| 文件 | 入口／职责 |
| --- | --- |
| `tools/SshWorkbench/main.cpp` | 定位公司端配置、应用统一主题、组装窗口 |
| `tools/SshWorkbench/WorkbenchWindow.cpp` | 项目编辑、批量打开、标签状态与关闭确认 |
| `tools/SshWorkbench/SshSession.cpp` | 后台创建本地 ConPTY、SSH 参数、输入输出、尺寸与退出 |
| `tools/SshSetup/EnvironmentService.cpp` | `prepareSshArguments` 复用已核对主机指纹并限制 SSH 行为 |
| `tools/ProjectLauncher/LauncherCore.cpp` | 复用项目配置验证与原子保存 |
| `apps/RemoteClient/terminal/TerminalView.cpp` | 复用离线 xterm.js、中文输入、剪贴板与字号 |
| `platforms/windows/ConPty/ConPtyProcess.cpp` | 复用 Win32 进程、管道与官方 ConPTY 运行库 |
| `tools/SshWorkbench/Build.ps1` | 编译、测试、Qt WebEngine／ConPTY 部署与压缩包 |

调用方向：窗口 → SshSession → ConPtyProcess → 系统 ssh.exe → 家庭 PowerShell → Claude。没有接入 Agent 协议、会话数据库或控制权租约。

远程目录单独编码成数据，通过 `Set-Location -LiteralPath -ErrorAction Stop` 切换；失败直接退出，不在其他目录启动 Claude。SSH 连接复用严格主机校验，禁止静默接受变化的指纹。临时信任文件保持到会话结束，私钥内容不进入配置与日志。

## 验证与边界

自动测试包括临时配置迁移、无自动会话启动、非法配置保护、真实本地 ConPTY/FakeCli 中文收发与尺寸同步、双会话隔离、退出／停止，以及错误目录不启动 CLI 的本地 PowerShell 替身验证。终端视图复用测试覆盖 UTF-8、ANSI、剪贴板、输入门控、尺寸与输出溢出。

真实双端验收仍需：两个不同项目同时打开 Claude，中文输入法、`/`、Tab、Esc、方向键、Ctrl+C、复制粘贴、窗口缩放；错误目录、未授权密钥、超时及指纹变化。首次向导的连接测试成功不等于已完成上述交互验收。

不实现断线续跑、原进程恢复、图片剪贴板上传、NAS/Git 集成。关闭时后台停止 SSH 并清理资源；主机探测尚未返回时可能需要等待其有限超时，但界面应保持响应。

### 2026-10-18 本机验证记录

- Debug／Release 构建及各 7 组 CTest 通过，启用 `/W4 /WX /analyze`。
- Debug 最新会话测试 11 项通过，包含页面输入中文 → ConPTY/FakeCli → 页面回显的整链路；界面测试 8 项通过，包含关闭确认期间新会话启动的竞态。
- 终端测试等待 Qt 窗口焦点后再发送快捷键，避免测试窗口切换时向空指针发送按键。测试初始化关闭 Chromium 后台计时降频；未修改生产终端的安全设置。
- 实际检查 100%／150% 预览布局；截图模式不连接远端。
- 携带包在 PATH 不含 Qt 安装目录时，工作台预览启动和 WebEngine 中文／ANSI 渲染检查返回 0。渲染探针临时使用 QtTest DLL，发布压缩包不依赖测试程序。
- 原 RemoteClient／RemoteAgent 的 Release 目标仍可独立构建。真实远端 Claude 双项目交互尚待用户验收，不能用本地 FakeCli 测试代替。
