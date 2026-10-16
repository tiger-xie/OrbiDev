# RADManager · Codex 分阶段开发任务清单

> 与 `01_RADManager_完整开发计划.md`、`02_AGENTS.md` 配套。每一张任务卡都应在独立分支完成、验证、审查后再合并。

**开发基线**：Qt 5.15.2 MSVC x64 + VS 2022 / v143 + C++20 + CMake；本机 Windows 10 IoT 企业版 LTSC x64（19044）首验。TASK-M0-01 使用纯 Win32，不链接 Qt；Qt 路径与构建配置见主计划第 3 节。

## 通用交付格式

- 目标功能与外部可见行为
- 所新增/修改文件列表
- 对公共接口及协议的影响
- 构建命令与实际测试结果
- 人工复现/验收步骤
- 已知限制、风险及后续改进

### TASK-M0-01 — 技术预研：Windows ConPTY

**目标**：纯 C++20 创建 ConPTY，运行 FakeCli，支持收发 VT、中文、窗口 Resize、正确释放资源。

**实现**：准备 ConPTY wrapper；FakeCli 支持 `echo`、ANSI 颜色、Unicode、逐行高吞吐输出、方向键测试。

**验收**：读取高流量不死锁；输入中文显示正确；resize 有效；退出不泄漏句柄。

**当前记录（2026-10-17）**：已获准接入微软官方 ConPTY 运行库 1.25.260930003，正式工程 Debug / Release 各 6/6 测试通过，待阶段审查。原生系统 API 句柄增长仍可由独立探针复现。详见 [M0-01 验证记录](docs/M0-01_ConPTY技术验证.md)；不代表 M0-02 或整个 M0 已完成。

**给 Codex 的指令**：

> 阅读三份设计文档，只执行 TASK-M0-01。先说明 ConPTY 的 HANDLE 生命周期和输入/输出线程模型，再实现最小 POC 和 FakeCli。暂时不接 Qt、网络、Claude，也不做 UI。编译、运行并记录结果。

### TASK-M0-02 — 技术预研：Qt + xterm.js

**目标**：本地 Qt QWebEngineView 通过 QWebChannel 调用 xterm.js，输入输出数据往返，识别 VT、中文、Ctrl+C 和尺寸变化。

**验收**：本地调试窗能打开；输入/输出功能正确；完全离线加载前端资源；视觉效果接近真实终端。

**工具链补充**：实际验证 Qt 5.15.2 + VS 2022/C++20 的编译、moc、链接及启动；覆盖中文输入法、Ctrl+C、方向键、复制粘贴、resize、全屏重绘和高吞吐输出，通过后锁定 xterm.js/FitAddon 版本。

**当前记录（2026-10-17）**：独立离线 TerminalPoc 已实现，固定 xterm.js 4.19.0 / FitAddon 0.5.0；本机 Debug / Release 各 7/7 自动测试通过，包含原 ConPTY 回归，同机部署目录启动通过。用户已确认 M0-02 验收通过并要求提交 Git；未提供逐项操作明细，详见 [M0-02 验证说明](docs/M0-02_Qt终端技术验证.md)。

**给 Codex 的指令**：

> 只实现 TASK-M0-02。编写独立的 Qt WebEngine/xterm.js 演示，使用本地模拟终端输出。不要连接远程 Agent。验证中文输入、复制粘贴、resize、ANSI。

### TASK-M0-03 — 家庭网络预研

**目标**：检查既有 OpenWrt Fail Closed 规则与家中/公司 Tailscale 的连通性；确认合法授权的端口访问路径。

**验收**：家中测试服务通过私网连接；故障时不会意外走未授权出口；原家用 NAS 可正常访问。

**边界**：不在软件中集成代理、改写家庭主路由或实现账号地区规避功能。

### TASK-M1-01 — CMake 工程骨架

**目标**：`RemoteClient.exe`、`RemoteAgent.exe`、`RADCore`、`RADProtocol`、`RADTests` 可编译；引入 CTest/Qt Test。

**验收**：全新 build 可从根目录构建；测试命令能执行；目录遵循主计划。

**部署补充**：独立部署目录能启动最小 Client/Agent，检查 WebEngine 辅助进程、资源/locales、Qt 平台插件及 SQLite 驱动；不提前实现真实终端或业务持久化。已安装依赖与实际验证结果分别记录。

**给 Codex 的指令**：

> 只执行 TASK-M1-01。创建设计文档所规定的基础目录、CMake targets、最小启动入口、Mock 接口和测试。不要实现真实网络、ConPTY 或界面逻辑。确保两 EXE 和测试工程编译成功。

### TASK-M1-02 — 模型与接口定义

**目标**：定义 SessionRequest、LaunchSpec、ProviderCapabilities、SessionId、SessionStatus、ITerminalBackend、IProviderAdapter、ISessionManager、错误码。

**验收**：Mock 实现可链接并单测；接口没有依赖具体 Claude/Windows/WebEngine 模块。

### TASK-M2-01 — ConPTY backend 产品化

**目标**：将 POC 整合为 RAII `WinConPtyBackend`，完善线程、退出、输出、异常、终端 Resize 和进程树管理。

**验收**：连续创建/结束 100 次 FakeCli 无句柄泄漏或崩溃；3 并发高输出无互相阻塞。

### TASK-M2-02 — SessionManager

**目标**：创建、列出、附加、分离、终止、会话进程状态；控制权租约；并发限制。

**验收**：测试可证明 detach 不 kill、terminate 显式生效、不同 session 输入互不串扰。

### TASK-M3-01 — 协议 DTO 与编解码

**目标**：实现 RADP/1 JSON envelope、二进制帧、UUID 校验、版本协商、最大帧大小、错误协议。

**验收**：单元/模糊输入测试覆盖非法长度、损坏包、错误版本、不存在 session。

### TASK-M3-02 — Agent WebSocket Server

**目标**：提供经鉴权的 create/list/attach/detach/terminate/resize、I/O 转发、保活与重连。

**验收**：仅授权客户端可访问；故意断 socket 不会终止 CLI；输出队列有上限。

### TASK-M4-01 — Qt Client 基础界面

**目标**：主机状态、项目树、会话列表、QTabWidget、新建/分离/终止确认。

**验收**：不连接真实 Claude 时能通过 FakeAgent 验证 UI 操作和错误反馈。

### TASK-M4-02 — TerminalView 集成

**目标**：Qt WebEngine 中显示 xterm.js；支持输入/ANSI/中文/复制/尺寸同步。

**验收**：用远程 FakeCli 完整交互；终端字节透传无额外编码损坏。

### TASK-M5-01 — 断线恢复

**目标**：稳定 SessionId、客户端重新连接列出原进程、附加；维护有限原始输出并提示超限。

**验收**：断网 1 分钟 CLI 继续运行，恢复网络后可以输入；长日志无法精确重建时明示限制。

### TASK-M5-02 — SQLite 与运行诊断

**目标**：会话元数据存储、Agent 重启后状态迁移、退出码、错误分类、诊断日志与轮转。

**验收**：Agent 崩溃恢复不会谎称原 PID 仍在；无密钥泄漏；迁移版本可测试。

### TASK-M6-01 — Claude Provider

**目标**：检测 Claude CLI 安装版本/实际命令路径；从允许的项目路径启动交互会话；能力发现。

**验收**：可运行至少 2 个 Claude CLI（在用户服务使用资格符合要求前提下）；Agent 不复制 Claude 登录凭据；用户能够看到交互式审批提示。

**给 Codex 的指令**：

> 只执行 TASK-M6-01。实现 ClaudeProvider 的可执行文件探测和参数构造，保持 ITerminalBackend 对 Claude 零依赖。不复制认证信息；不使用 skip-permissions 类绕过选项。使用 Mock Provider 单测与本机真实手动验收。

### TASK-M7-01 — 安全加固

**目标**：Tailscale 端口访问、安全配对、工作区白名单、输入上限、速率限制、DPAPI 凭据和 Windows 防火墙检查。

**验收**：未配对拒绝、非法工作区拒绝、超大消息拒绝、不同会话访问控制有效。

### TASK-M7-02 — 压力与故障注入

**目标**：3-5 并发、网络频繁断线、终端大量输出、手动终止、Agent 重启、慢客户端背压。

**验收**：无严重资源泄漏、无主线程卡死、无输入串会话、重要错误可诊断。

### TASK-M8-01 — 产品打包和部署文档

**目标**：Windows 双端部署包、版本兼容、日志查看、升级/回滚、首次配对说明、网络问题诊断。

**验收**：从未装过 RADManager 的两台测试机能够按文档安装使用，且无需上传代码到第三方云。

### TASK-M9-01 — Codex Provider（V1.1）

**目标**：在不修改 ConPTY/SessionManager 公共接口的情况下接入 Codex CLI。

**验收**：一个 Claude 和一个 Codex 会话可以并行，互相隔离。

### TASK-M9-02 — Git Worktree（V1.1）

**目标**：工作区多实例隔离、列出状态、需确认的创建/清理工作树。

**验收**：同仓库两个会话修改不同分支不互相覆盖，未提交数据不会被静默删除。

### TASK-M9-03 — Git/Build 集成（V2）

**目标**：浏览 Diff、发起授权的 Git 操作、通过 Gitea 或公司 Runner 触发构建、收集日志。

**验收**：远程构建异步返回，客户端掉线不影响构建，日志可再次查看。

---

## 推荐实施方法

- 用户（架构负责人）先审每阶段公共接口，再让 Codex 写功能代码。
- 每张任务卡一个工作分支，完成后编译、单测、人工验证并提交审查。
- 不要通过“增加大量 TODO 空接口”假装功能已经完成。
- 如 API 或设计必须变更，先写 `docs/ADR/` 记录理由及影响后再实现。
