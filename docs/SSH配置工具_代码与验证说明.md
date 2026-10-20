# SSH 配置工具：代码与验证说明

实现位于 `codex/ssh-setup-wizards` 分支。用户原有未提交代码保留；本阶段只新增独立工具，不接入旧 Agent 协议或会话持久化。

使用方法见 [首次配置与家庭授权](SSH首次配置与家庭授权_使用说明.md)。以下路径相对仓库根目录。

## 源码入口

| 文件 | 入口 | 职责 |
| --- | --- | --- |
| `tools/SshSetup/main.cpp` | `main` | 两个 EXE 共用入口；家庭版编译宏或 `--home` 选择模式 |
| `tools/SshSetup/SetupWindow.cpp` | `SetupWindow`、`runOperation` | 四步向导、操作确认、后台调用和错误反馈 |
| `tools/SshSetup/SetupTheme.cpp` | `applySetupTheme` | 集中维护石墨灰／青绿颜色、字体和控件状态 |
| `tools/SshSetup/IdentityService.cpp` | `ensureIdentity`、`inspectIdentity` | 当前用户专用身份、ACL 检查、公私钥匹配、指纹 |
| `tools/SshSetup/AuthorizationService.cpp` | `validatePublicKey`、`inspectHomeEnvironment`、`authorizeCurrentUser` | 公钥校验、服务识别、确认快照、授权文件追加与回滚 |
| `tools/SshSetup/EnvironmentService.cpp` | `testConnection`、`loadProfile`、`importConnectionInfo` | 分层连接检查、主机指纹核对、公开配置导入保存 |
| `tools/SshSetup/Build.ps1` | 构建脚本 | 独立 CMake、CTest、Qt 部署、压缩包 |

依赖仍为现有 Qt 5.15.2（Core／Widgets／Network／Test）及 Windows 系统 API，不引入新的第三方包。界面依赖服务库，服务库不引用 Widgets；旧项目不依赖此工具。

## 关键调用与数据

公司端：环境检查 → 专用目录安全创建 → 系统 `ssh-keygen` 生成或校验现有身份 → 展示公钥 → 家庭授权 → 连接验证 → 原子保存 `connection.json`。

家庭端：检测当前用户与运行中的 sshd → 校验单行 Ed25519 → 展示目标文件和指纹 → 用户确认 → 重检并比较 `HomeEnvironment` → 备份 → 追加并设置 ACL → 失败恢复 → 导出公开 JSON。

服务 API 为同步、有限时长操作；界面用 `QThread::create` 执行，不在主线程等待进程。操作期间禁用步骤切换与关闭，结束后在 UI 线程更新结果。

`ConnectionProfile` 保存 host、user、port、identityFile 路径、hostFingerprint；不存储私钥正文。家庭导出不包含 identityFile。导入公开 JSON 不允许覆盖本机身份，旧项目表和 terminal_path 完全不改写。

SSH 测试先解析本机别名，再检查 TCP／SSH 标识；实际探测禁用额外配置身份与 agent。固定 PowerShell 检查 `Get-Command claude`，不启动 CLI。指纹非空时必须经界面核对，并与扫描到的主机公钥一致；未知或变化的主机不自动信任。

SSH-05 将连接结果拆成网络端口、SSH 服务、主机信任、认证、PowerShell、Claude 命令六个阶段。`classifySshProbeResult` 只在内存中分析 OpenSSH 输出；`formatConnectionReport` 按固定状态和错误类型生成脱敏文字，向导与主机编辑窗口共用。未到达的阶段显示“未执行”，无法确定时显示“未能确认”，不因前置失败把所有阶段标为失败。修改连接目标后清除旧结果，必须重新检测。

家庭端 `homeEnvironmentReport` 只导出服务状态、账户类型及检查结论，不复制主机名、用户名、地址、路径或原始错误。界面仍展示实际主机、用户和服务程序路径供本人核对。`sshListenConfigurationWarning` 仅静态检查监听配置；遇到 Include 或复杂规则保守提示人工检查，不声称已经确认运行中的端口，也不修改配置或重启服务。

## 回归重点

- `IdentityServiceTests`：真实临时密钥生成与复用、残缺／错配、过宽目录、并发锁、非法路径、已有管理员 ACL。
- `AuthorizationServiceTests`：非法输入／私钥／附带命令、标准普通／管理员配置、去重保留、权限失败恢复、新文件失败清理、预览后环境变化。
- `EnvironmentServiceTests`：参数注入拒绝、配置保存／导入、身份不被外部 JSON 替换；可选真实连接使用 `ORBIDEV_LIVE_SETUP_CONFIG`，默认跳过。
- `SetupWindowTests`：初始步骤不能跳过、无副作用预览、私钥误粘贴无法进入授权步骤。

SSH-05 补充：指纹误作公钥的具体提示、仅回环监听／复杂配置、家庭报告与连接报告脱敏、未知／变化主机密钥、认证失败、探测超时、PowerShell 与 Claude 缺失，以及目标变更使复制结果和可见反馈失效。服务脚本测试仍只模拟确认／取消及服务状态，不触发实际 UAC。

构建启用 `/W4 /WX /analyze`。验证证据在忽略的 `build-ssh-setup` 中，包括 CTest 日志、实际窗口截图和可携带压缩包；截图为 Qt 窗口渲染，不能证明真实 UAC 或家庭授权已完成。

2026-10-18 本机记录：Debug／Release 构建及各 4 组 CTest 通过；clang-format 检查与 PowerShell 语法检查通过。实际 Qt 窗口检查了 100%／150% 缩放。携带包在 PATH 不包含 Qt 安装目录的条件下，两端 EXE 预览启动均返回 0。

另行启用只读双端测试：现有身份连接家庭电脑成功，远端能找到 Claude；经已信任连接读取家庭主机公钥后，正确指纹测试成功，改错指纹被拒绝。对应 `live-connection-debug.txt`。未创建真实公司身份、未修改家庭授权、未启动 Claude；不能据此声称新公钥配对或 UAC 已验收。

## 边界和维护约束

不把本工具扩展成常驻配对服务。已有 sshd 不重启、不修改默认 Shell；缺少 Server 时提供安装入口和说明，自动系统安装／防火墙配置尚未实现。

未知授权规则停止自动处理；管理员共享授权文件提示不可移除。应用前必须保持预览快照校验；指纹变化不允许退化成忽略校验。授权文件备份应保留到用户确认连接正常，程序不自动清除。

原位授权写入保留所有者及已有权限，再设置目标 ACL；失败回滚已覆盖自动测试，但突然断电不保证事务完整，需从备份恢复。真实家庭账户、UAC 与新公钥登录必须分别验收。

2026-10-18 用户家庭测试机验收：截图确认新公钥授权后，网络、SSH 服务、密钥认证与远端 Claude 可用性检查均通过；未启动实际 Claude 会话。启动脚本提供用户确认及 UAC，自动化仅模拟服务状态与取消分支。

## SSH-05 本轮验证（2026-10-20）

- Release 构建通过，沿用 `/W4 /WX /analyze`；修改的 C++ 文件通过 clang-format 检查，`git diff --check` 通过。
- 最终配置工具 CTest 4/4 通过，覆盖新增诊断与目标修改后的反馈失效；服务启动脚本的 4 个模拟测试通过，没有实际启动服务或请求 UAC。
- 工作台全套首轮 7/10 通过。SshSession 和终端测试出现既有初始化超时；HostDialog 的系统剪贴板访问报 `0x800401d0`。不能据此标记完整回归通过。
- 单独复测 SshSession 通过；终端测试 10 个通过、1 个失败，初始化超时未再次出现，剩余失败为同一系统剪贴板访问错误。复跑通过不表示初始化偶发问题根因已修复。
- HostDialog 最终在原生 Windows 平台为 8 个测试通过、复制诊断测试失败；有界重试后系统剪贴板仍不可用。显式加载 Qt offscreen 插件后 9/9 通过，仅证明按钮、报告及失效逻辑，不替代系统剪贴板手工验收。
- 实际 Qt 向导截图检查了 100%／150% 的家庭环境、公钥展示和连接页面；较长内容通过滚动区展示。两端配置 EXE 在 PATH 不含开发 Qt 的环境下预览启动均返回 0。
- 已把本次构建的配置 EXE 同步到 `build-ssh-workbench/Release` 和 `build-ssh-setup/Release`，根目录启动脚本可启动新版；现有分发 ZIP 未重新打包。

本轮未启用真实远端连接测试，未执行系统安装、实际 UAC、修改家庭授权或新身份配对。普通／管理员配置、自定义规则拒绝与授权回滚沿用自动回归；真实新环境仍需分别验收。Debug 的 SSH-KI-001 按用户要求继续延期，不因 Release 构建成功标记修复。
