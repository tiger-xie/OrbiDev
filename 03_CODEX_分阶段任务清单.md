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

**当前进度（2026-10-17 更新）**：用户已准备家庭测试电脑，确认 Claude CLI 可用且 Tailscale 已连接；先以该机进行远程 CLI 联调。公司端到家庭端的路径和 Agent 端口仍待实测；NAS 尚未安装，正式住宅出口/Fail Closed 场景及 NAS 验收延期，不阻塞终端链路开发。见 [M0-03 网络预研记录](docs/M0-03_家庭网络预研.md)。

### TASK-M1-01 — CMake 工程骨架

**目标**：`RemoteClient.exe`、`RemoteAgent.exe`、`RADCore`、`RADProtocol`、`RADTests` 可编译；引入 CTest/Qt Test。

**验收**：全新 build 可从根目录构建；测试命令能执行；目录遵循主计划。

**部署补充**：独立部署目录能启动最小 Client/Agent，检查 WebEngine 辅助进程、资源/locales、Qt 平台插件及 SQLite 驱动；不提前实现真实终端或业务持久化。已安装依赖与实际验证结果分别记录。

**给 Codex 的指令**：

> 只执行 TASK-M1-01。创建设计文档所规定的基础目录、CMake targets、最小启动入口、Mock 接口和测试。不要实现真实网络、ConPTY 或界面逻辑。确保两 EXE 和测试工程编译成功。

**当前记录（2026-10-17）**：M1-01 骨架已实现；全新目录 Debug/Release 编译、各 10/10 CTest 和同机独立部署烟测通过，待审查。未执行人工 VS 断点验收；Debug WebEngine 退出诊断及部署限制见 [M1-01 验证记录](docs/M1-01_工程骨架与验证.md)。模型及 Mock 接口留给 M1-02，整个 M1 尚未完成。

### TASK-M1-02 — 模型与接口定义

**目标**：定义 SessionRequest、LaunchSpec、ProviderCapabilities、SessionId、SessionStatus、ITerminalBackend、IProviderAdapter、ISessionManager、错误码。

**验收**：Mock 实现可链接并单测；接口没有依赖具体 Claude/Windows/WebEngine 模块。

**当前记录（2026-10-17）**：模型及三个抽象接口已实现，13 个契约测试方法 Debug/Release 均通过，Release 全套 11/11 通过；Debug Client 曾一次超时、单独复跑通过，详见 [M1-02 说明](docs/M1-02_核心模型与接口.md)。当前只验证内存替身，真实会话线程及资源行为留给 M2；M1-02 待用户审查。

### TASK-M2-01 — ConPTY backend 产品化

**目标**：将 POC 整合为 RAII `WinConPtyBackend`，完善线程、退出、输出、异常、终端 Resize 和进程树管理。

**验收**：连续创建/结束 100 次 FakeCli 无句柄泄漏或崩溃；3 并发高输出无互相阻塞。

**当前记录（2026-10-17）**：WinConPtyBackend 已实现；Debug/Release 后端 7/7 通过，预热后 101 次真实进程严格句柄等量、3 并发及进程树清理通过。全套回归的旧 WebEngine POC 存在超时，Release 单独复跑通过、Debug 仍超时，不标记 Debug 全套通过。详见 [M2-01 说明](docs/M2-01_Windows终端后端.md)。用户已批准继续 M2-02；M2-01 已提交为 `9df7806`。

### TASK-M2-02 — SessionManager

**目标**：创建、列出、附加、分离、终止、会话进程状态；控制权租约；并发限制。

**验收**：测试可证明 detach 不 kill、terminate 显式生效、不同 session 输入互不串扰。

**当前记录（2026-10-17）**：SessionManager 已实现创建/查询、独立订阅、单写者租约、并发限制和显式终止。新增 9 个单元测试方法及 1 个真实三会话集成方法 Debug/Release 均通过；Release 全套 20/20 通过。已验证全部分离后继续运行、同 ID 重附加与输入隔离。详见 [M2-02 说明](docs/M2-02_SessionManager.md)。用户已批准提交并继续后续任务；M2-02 与调试修复已提交为 `1991340`。未接入远程网络或真实 Claude。

### TASK-M3-01 — 协议 DTO 与编解码

**目标**：实现 RADP/1 JSON envelope、二进制帧、UUID 校验、版本协商、最大帧大小、错误协议。

**验收**：单元/模糊输入测试覆盖非法长度、损坏包、错误版本、不存在 session。

**当前记录（2026-10-17）**：RADP/1 DTO、JSON/二进制编解码、UUID/版本/帧大小校验、安全错误及独立会话存在性检查已实现；11 个测试方法与 3000 轮定种子变异输入 Debug/Release 均通过。详见 [M3-01 说明](docs/M3-01_协议编解码.md)。用户已批准提交并继续 M3-02，M3-01 已提交为 `f648917`；该提交本身不包含网络监听与身份认证。

### TASK-M3-02 — Agent WebSocket Server

**目标**：提供经鉴权的 create/list/attach/detach/terminate/resize、I/O 转发、保活与重连。

**验收**：仅授权客户端可访问；故意断 socket 不会终止 CLI；输出队列有上限。

**当前实现（2026-10-17）**：经设备令牌摘要鉴权的 WebSocket 路由、单身份连接、会话 I/O、失联清理和有界输出已实现；Agent 显式 --config 启动固定 FakeCli 服务，工作区 ACL 继续生效。Debug 本机网络集成 10 个方法、配置/Provider 2 个方法及实际 EXE 启动链路通过，完整结果见 [M3-02 说明](docs/M3-02_AgentWebSocket服务.md)。用户已批准提交并继续 M4-01；M3-02 已提交为 `9031644`。跨机器 Tailscale、Claude、配对 UI/DPAPI 与持久化日志尚未验收。

### TASK-M4-01 — Qt Client 基础界面

**目标**：主机状态、项目树、会话列表、QTabWidget、新建/分离/终止确认。

**验收**：不连接真实 Claude 时能通过 FakeAgent 验证 UI 操作和错误反馈。

**当前实现（2026-10-17）**：M4-01 原生深色界面、FakeAgent 异步操作、多标签与新建/分离/终止确认已实现；集中主题及主题标识为后续换肤预留。Debug/Release 构建及 11 个 UI 行为方法通过，Release 全套 25/25 通过；已查看 100%/125%/150% 实际截图，缩放边界见 [M4-01 说明](docs/M4-01_QtClient基础界面.md)。M4-01 已按用户要求提交为 `f3c9231`；真实终端和远程连接由下一张 M4-02 实现。

### TASK-M4-02 — TerminalView 集成

**目标**：Qt WebEngine 中显示 xterm.js；支持输入/ANSI/中文/复制/尺寸同步。

**验收**：用远程 FakeCli 完整交互；终端字节透传无额外编码损坏。

**交互补充**：覆盖 `/`、Tab、Esc、方向键和焦点切换，Tab 在终端获得焦点时传入 CLI，不被 Qt 导航截走；真实 Claude 菜单和补全在 M6-01 验收。

**当前实现（2026-10-18）**：已实现认证 WebSocket 客户端、离线 xterm/WebChannel 与真实 FakeCli 多标签交互，复用现有协议及依赖。Debug 本机回环三标签端到端通过，包括中文输入法提交事件、ANSI、复制粘贴、连续 resize 与关闭窗口后进程存活；完整构建/测试及限制见 [M4-02 说明](docs/M4-02_远程终端集成.md)。用户已批准提交并继续 M5-01；交互模式及输入修复已通过 Release 29/29、Debug 相关 4/4 测试。家庭 Tailscale、真实输入法候选窗和 Claude 仍待分别验收，不将本机回环视为跨机器验证。

### TASK-M5-01 — 断线恢复

**目标**：稳定 SessionId、客户端重新连接列出原进程、附加；维护有限原始输出并提示超限。

**验收**：断网 1 分钟 CLI 继续运行，恢复网络后可以输入；长日志无法精确重建时明示限制。

### TASK-M5-02 — SQLite 与运行诊断

**目标**：会话元数据存储、Agent 重启后状态迁移、退出码、错误分类、诊断日志与轮转。

**验收**：Agent 崩溃恢复不会谎称原 PID 仍在；无密钥泄漏；迁移版本可测试。

### TASK-M6-01 — Claude Provider

**目标**：检测 Claude CLI 安装版本/实际命令路径；从允许的项目路径启动交互会话；能力发现。

**交互补充**：真实验证 `/` 命令菜单、Tab 补全、Esc 取消、方向键选择及审批提示；允许先在本机验证，跨端验收另记。

**验收**：可运行至少 2 个 Claude CLI（在用户服务使用资格符合要求前提下）；Agent 不复制 Claude 登录凭据；用户能够看到交互式审批提示。

**给 Codex 的指令**：

> 只执行 TASK-M6-01。实现 ClaudeProvider 的可执行文件探测和参数构造，保持 ITerminalBackend 对 Claude 零依赖。不复制认证信息；不使用 skip-permissions 类绕过选项。使用 Mock Provider 单测与本机真实手动验收。

### TASK-M6-02 — 图片附件与预览

**目标**：客户端粘贴/拖入图片、缩略图/独立预览，确认后上传到 Agent 受控附件目录，以实际 Claude CLI 支持的方式引用。

**验收**：先在本机验证 CLI 图片引用，再验证双端传输；图片内容一致、权限与会话隔离正确，超限/非法类型/越界路径拒绝，失败与取消可见，附件按配额和期限清理。不得把客户端路径直接发给远端当作可用文件。

**边界**：实施前补充协议设计及测试；本任务不承诺 xterm.js 内嵌图片显示，旧 WebEngine 的图片处理兼容与安全仍需验证。M1 不提前实现附件接口。

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
