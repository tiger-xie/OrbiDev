# Remote AI Development Manager（RADManager）完整开发计划

> 版本：架构规划 v1.1 ｜ 编制：2026-10-16
> 目标读者：架构负责人、Codex 编程代理、Qt/C++ 实现工程师
> 适用：家庭 Windows 开发笔记本运行 Claude Code CLI；公司 Windows 电脑远程多标签操作；NAS 上的 Gitea 托管源码。
> **本文是开发规格，不是已完成的软件；示例协议和接口需要按照本文先测试、再实现。**

---

## 0. 一页决策摘要

**定位**：跨设备远程交互式 AI CLI 会话管理平台，不仅是远程 SSH 工具。公司端是 Qt 客户端，家庭端是 Windows Agent；可开多个终端标签，每个标签对应一个独立的 Claude Code/未来 Codex CLI 进程。实际文件修改、命令和模型认证全部在家庭端完成。

**技术基线**：Qt 5.15.2 MSVC x64 / Visual Studio 2022（v143）/ C++20 / CMake；首要开发和运行验收环境为本机 Windows 10 IoT 企业版 LTSC x64（19044），Windows 11 列入后续兼容测试。公司端 Qt Widgets + QWebEngineView + xterm.js；Agent 采用 Win32 ConPTY + Qt WebSockets；跨网通过 Tailscale；持久化 SQLite；第一版静态注册 AI Provider 工厂。xterm.js 与 FitAddon 经本机 POC 验证后锁定版本。

**技术基线修订范围**：统一工具链和验收基线，保留原有架构与里程碑顺序；不变更公共接口、协议或业务功能，不新增依赖。文档版本 v1.1 不表示产品已进入 V1.1 功能阶段。配套 `AGENTS.md`、`02_AGENTS.md`、`03_CODEX_分阶段任务清单.md` 和 `04_给Codex的首次启动指令.md` 已同步本机技术基线。

**必须优先做到**：多终端、真实交互（中文/ANSI/终端尺寸）、客户端断线后进程仍运行、按 ID 重新附加、工作目录隔离、严格认证与工作区白名单、显式终止、基本错误处理。

**不应误解**：`claude --resume` 恢复聊天上下文，不等于重新附加任意仍在运行的 Windows ConPTY 进程；Agent 进程或系统重启，原 ConPTY 进程一般无法按相同句柄恢复。会话“脱离客户端”与“脱离 Agent”不同。

**平台合规前置条件**：仅在服务提供商允许的实际地区、账号及组织条件下使用 Claude；Tailscale、软路由、静态住宅出口仅解决合法的网络连接问题，不是改变使用资格的手段。

## 1. 业务需求与边界


**ConPTY 依赖决策（2026-10-17）**：已获准固定使用微软官方 Microsoft.Windows.Console.ConPTY 1.25.260930003，随应用部署运行库以解决本机系统 API 的句柄增长；版本校验、离线配置、许可和实测范围见 [M0-01 验证记录](docs/M0-01_ConPTY技术验证.md)。此依赖不改变 Qt/C++ 基线，系统原生探针保留用于诊断。

### 1.1 使用场景

- 家庭 Windows 笔记本：已安装 Claude Code，拥有实际源码项目、Qt/MSVC 工具链及 Git 凭据；保持开机，Agent 随当前开发用户登录而启动。
- 公司 Windows 电脑：运行 RADManager 客户端；通过企业允许的 Tailscale 网络连接家庭 Agent；无需安装 Claude Code 或复制其认证信息。
- NAS：在原家庭路由器下运行 Gitea 和 Tailscale。家庭笔记本在局域网中 Git Push；公司电脑通过 NAS 的 Tailscale 地址 Git Pull、编译。
- 项目可能有多个仓库；同一仓库中的并行任务应采用独立 Git worktree 或明确标记为危险的共享目录模式。

### 1.2 MVP P0 功能

1. 家庭 Agent 启动、日志、检测 Windows/Claude 可执行文件、端口监听、授权连接。
2. 公司客户端新增家庭主机连接配置、在线状态、重连。
3. 会话列表与新建：名称、Provider、工作区、命令参数、终端尺寸。
4. 多标签页终端；每个会话独立 ConPTY + 子进程；建议 3 个并发会话验收，最大并发数量可配置（默认 4，上限由用户设置）。
5. 完整终端输入输出：UTF-8、ANSI/VT、颜色、光标、Ctrl+C、Enter、Tab、Esc、方向键、窗口缩放、中文输入与复制粘贴；`/` 命令菜单由 CLI 原样生成，客户端不另造菜单。
6. 会话分离/附加：关闭客户端或切换终端不得自动终止 Agent 的 CLI 进程；再次连接可按稳定 Session ID 查找、附加。
7. 终止会话需要显式操作及二次确认；展示退出码、状态、启动时间、目录和 Provider。
8. 保留日志并设置保留时间、大小配额；敏感信息默认不进入长期日志。
9. 项目目录白名单；认证、会话访问控制和安全默认配置。
10. 自动化测试：多实例并发、异常断线、输出高吞吐量、网络重连、Agent 意外退出等。

### 1.3 P1/P2 扩展功能（保留接口，不在 MVP 实现）

- P1：Codex CLI Provider、普通 PowerShell Provider、Git Worktree 自动创建、项目 Git 状态、全局搜索、快捷命令、会话重命名、资源监控。
- P1：Claude 自带后台会话的能力适配（若实际安装版本支持 `claude --bg / claude agents / claude attach`），但不影响平台独立的 ConPTY 模式。
- P2：Gitea 仓库浏览、Diff/Review、自动提交、Gitea CI / 公司构建 Runner、测试报告、通知中心、任务队列。
- P2：NAS 中继、Web/手机客户端、多人用户/权限、Linux PTY Provider、结构化 AI 任务 API 和插件市场。

### 1.4 V1 明确不做

- 不在公司电脑上保存家庭 Claude 登录 Token，不做远程传输凭据功能。
- 不将 Remote Agent 或 ConPTY 控制端口直接暴露到公网。
- 不一开始实现完整 IDE、编辑器、远程文件同步器、远程桌面。
- 不提供绕过服务商封禁、地区限制或身份核验的功能。
- 不保证家庭 Agent/Windows 重启后继续运行原 PTY 进程。
- 不自动执行危险 Git 操作（force push、reset --hard、删除 worktree）。

### 1.5 交互与图片补充需求（2026-10-17）

- M4-02 验证 Tab、Esc、方向键和 `/` 的终端字节透传及焦点行为，M6-01 在本机真实 Claude CLI 验证菜单、补全和审批提示，不把 FakeCli 通过视为 Claude 已通过。
- 新增 M6-02 图片附件与预览任务：客户端粘贴/拖入图片，预览并确认发送，上传至 Agent 受控附件目录，再通过实际 CLI 版本支持的方式引用。客户端本地路径和剪贴板不能直接作为远端可访问文件使用。
- 上传须经过认证、会话权限和大小/类型限制，限制目标路径并明确失败、取消及过期清理；协议变更在该任务实施前单独设计和测试，不在 M1 提前增加文件传输接口。
- 图片预览采用客户端独立组件；终端内嵌图片协议支持需另行实测，不承诺当前 xterm.js 能显示任意图片输出。不自动打开 CLI 输出中的任意文件路径或 URL。
- RADManager 仅管理终端会话与输入输出，认证仍由官方 CLI 处理；不提取凭据、不建立模型训练数据采集功能。

## 2. 网络与部署拓扑

**当前联调安排（2026-10-17）**：家庭测试电脑已由用户准备，用户确认 Claude Code CLI 可用且 Tailscale 已连接。先走“公司 Client → Tailscale → 家庭测试电脑 Agent → Claude CLI”；双端连通、Agent 端口和应用鉴权仍需实测。NAS 尚未安装，以下 NAS/Gitea 及正式网络拓扑保留为目标部署，不是当前已具备的环境。NAS 不参与终端输入输出，也不作为本轮联调前置条件；正式住宅出口及相关 Fail Closed 场景另行验收。


```text
公司 Windows PC
  └─ RADManager Client (Qt)
       └─ Tailscale 私有连接 / 端口访问授权
            └─ 家庭 Windows 笔记本 (Dev-Work Wi-Fi)
                 ├─ RemoteAgent.exe (普通开发用户会话)
                 │    ├─ SessionManager
                 │    ├─ ConPTY #1 -> Claude CLI -> Repo A / Worktree A
                 │    ├─ ConPTY #2 -> Claude CLI -> Repo B / Worktree B
                 │    └─ ConPTY #3 -> Claude CLI -> Repo A / Worktree C
                 ├─ Claude / Qt / MSVC / Git
                 └─ LAN Git Push -> NAS Gitea

原家庭主路由 LAN ─── NAS Gitea + Tailscale
       └─ RAX3000Me WAN ── Dev-Work Wi-Fi / Daily-Fast Wi-Fi
公司 PC ──────────────── Tailscale -> NAS Git Pull / 公司本地编译
```

网络原则：原主路由不改动，家里 NAS 不经开发代理；OpenWrt 为开发 Wi-Fi 提供单独子网。家庭开发笔记本必须能够在既有 Fail Closed 防火墙策略下**受控地**访问 Tailscale 所需路径；不可假设 SOCKS5 线路对 Tailscale 的 UDP/TCP 连接天然可用。

**部署验证顺序**：

1. 家中 Agent 先在 `127.0.0.1` 建立可用的本地客户端连接。
2. 在家庭局域网内验证客户端到 Agent 的安全访问（仅临时测试）。
3. 在家庭/公司两端已获准使用 Tailscale 的前提下，确认两设备可互相访问 TCP 指定端口。
4. 固定 Agent 对 Tailscale IP 的监听与 Windows 防火墙规则；禁止全网监听+公网映射。
5. 如果 Dev-Work 故障阻断妨碍 Tailscale，优先诊断 OpenWrt 路由与明确授权路径；NAS 反向中继列为备选扩展，并不在 MVP 随意启用普通 WAN 直连。

## 3. 技术栈和依赖决策

| 层 | 建议技术 | 理由/限制 |
|---|---|---|
| OS | 本机 Windows 10 IoT 企业版 LTSC x64（19044）；Windows 11 后续兼容测试 | ConPTY 最低支持 Win10 1809；LTSC 生命周期应按具体版本核对，不套用普通 Windows 10 的支持期限。 |
| C++ | C++20 + VS 2022 / MSVC v143，统一 x64 | 保留现代 C++ RAII；先验证与 Qt 5.15.2 的编译、moc、链接和运行兼容性。 |
| Build | CMake，使用 VS 2022 生成器 | 本机已有 3.31.6；采用 target-based 配置，不依赖全局 include/link 设置。 |
| Qt | Qt 5.15.2 MSVC x64；Core、Widgets、Network、WebSockets、WebEngineWidgets、WebChannel、Sql、Test | 使用 Qt 5 API 与 `Qt5::` 构建目标，按组件实际职责链接。 |
| Terminal UI | xterm.js + FitAddon；SearchAddon 后续引入 | 经本机 WebEngine POC 验证后锁定版本及构建依赖；本地静态资源打包 QRC，不依赖 CDN，不默认最新版兼容。 |
| CLI Host | Win32 ConPTY | 不使用 `QProcess + QTextEdit` 假装完整终端。 |
| Control | QWebSocketServer/QWebSocket：版本化 JSON | 创建/列出/控制会话与认证。 |
| I/O | WebSocket binary frames | 传输 UTF-8/VT 原始字节，不把每个字节封装 JSON。 |
| Data | SQLite + JSON 配置（敏感数据经 Windows DPAPI/Credential Manager） | 小型单机 Agent 足够。 |
| Private net | Tailscale + 设备/端口级 Grants + Windows Firewall | 网络权限最小化；应用层仍须鉴权。 |
| Git | 本地 Git 命令（隔离封装） | 源码由家庭设备操作，NAS Gitea 作为仓库。 |

**许可证检查**：发布打包前确认 Qt WebEngine、xterm.js 与其他依赖的许可证、源码/notice 义务及你的商业使用方式。

### 3.1 本机工具链与构建约定

- 本机 Qt 安装路径为 `D:/Qt/5.15.2/msvc2019_64`。目录中的 `2019` 表示 Qt 预编译包的工具链来源，不要求应用使用 VS 2019；项目使用 VS 2022 / v143。MSVC 的有条件二进制兼容不能替代本项目的实际构建与运行验证。
- CMake 采用 C++20，并要求该标准生效；使用 Qt 5.15.2 的包配置和 `Qt5::Core` 等目标，不使用 Qt 6 专属 API 或构建命令。统一 x64，不混用 MinGW、32 位库或不同构建配置的运行库。
- Client 按需链接 Core、Widgets、Network、WebSockets、WebEngineWidgets、WebChannel；Agent 按需链接 Core、Network、WebSockets、Sql，不依赖 Widgets、WebEngine 或 WebChannel。测试目标使用 Qt Test；核心模块不依赖 UI。
- 本机路径仅进入开发者本地配置或构建参数，不硬编码到业务代码，也不要求其他机器沿用同一路径。

以下命令在相应阶段的 CMake 工程创建后执行，**不是已经通过的构建记录**。从 VS 2022 开发环境调用 CMake；若命令不在 PATH，可在 PowerShell 中设置仅当前会话有效的别名：

```powershell
Set-Alias cmake 'D:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
Set-Alias ctest 'D:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T v143 -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/msvc2019_64" -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

安装目录不同的机器应调整本地参数。已有 build 目录若使用其他生成器、架构或工具集，应选择新的构建目录，避免混用缓存。Debug 用于本机调试；Release 构建、测试和部署验证使用独立目录或对应配置，不能只验证 IDE 内启动。

### 3.2 WebEngine / xterm.js 兼容边界
**本地开发进度（2026-10-17）**：用户确认当前为公司电脑，家庭测试电脑已具备 Claude CLI 和 Tailscale，后续优先使用该机联调；NAS 与正式出口场景延期，双端网络仍未验收，不将整个 M0 标记完成。M1-01 工程骨架已完成本机 Debug/Release 构建和部署烟测，M1-02 已完成模型/接口及替身测试、待用户审查，实际会话实现和 VS 交互调试待验收。详见 [M1-01 验证记录](docs/M1-01_工程骨架与验证.md)。

**M0-02 当前实测（2026-10-17）**：独立离线终端已在本机 Qt 5.15.2 / Chromium 83 中验证，固定 xterm.js 4.19.0 + FitAddon 0.5.0；Debug、Release 自动测试及同机部署检查通过。用户已确认 M0-02 验收通过并要求提交 Git；该确认不等于整个 M0 或后续阶段已完成，具体人工操作明细未另行提供。详见 [M0-02 验证记录](docs/M0-02_Qt终端技术验证.md)。旧包已标记 deprecated，保留旧内核的兼容与维护风险评估。

Qt 5.15.2 的 WebEngine 基于较旧的 Chromium。M0 必须在本机验证 xterm.js 与 FitAddon 的实际组合，通过后记录版本并提交依赖锁文件；不得仅因前端构建成功就认定浏览器运行兼容。验证中文输入法、ANSI/VT、Ctrl+C、方向键、复制粘贴、窗口尺寸变化、全屏重绘和高吞吐输出。

终端页面及脚本随程序离线发布，仅加载受信任本地资源，限制外部导航和 QWebChannel 暴露范围。旧内核的安全维护风险不会因离线打包而消失，发布前需评估；若 POC 不通过，应记录失败证据并审查终端方案，不擅自升级 Qt 或替换技术栈。

### 3.3 环境状态与验证证据

技术基线检查时已确认本机 Qt 5.15.2 MSVC x64、VS 2022 / v143、CMake 3.31.6、Windows SDK 10.0.19041.0 / 10.0.26100.0，以及所需 Qt 模块、WebEngine 辅助进程和 SQLite 驱动文件。**安装存在不等于项目验证通过**。各阶段单独记录构建命令、测试结果、人工验收与未验证项；M0-01 的实现边界和验证记录见 [ConPTY POC 说明](docs/M0-01_ConPTY技术验证.md)，其通过不代表 Qt 界面或部署验收通过。

## 4. 分层架构与模块依赖

```text
apps/RemoteClient
    UI: HostList / ProjectExplorer / SessionTabs / Settings / TerminalView
    ClientController --> ITransportClient --> ProtocolV1
    TerminalView --> QWebEngineView + local xterm.js + QWebChannel

apps/RemoteAgent
    ServerBootstrap --> Authorization --> ControlRouter
    SessionManager --> IProviderAdapter
                   --> ITerminalBackend (Windows ConPTY)
                   --> ISessionRepository (SQLite)
                   --> IWorkspaceProvider (Local / GitWorktree)
                   --> IEventSink

core/ (纯接口、模型、版本化协议)
platforms/windows/ (ConPTY, DPAPI, Process/JobObject, paths)
providers/claude/ (启动参数与可用能力；不关心 UI)
providers/codex/ (V1.1)
extensions/git/ and extensions/build/ (以后)
```

关键依赖方向：上层依赖接口；核心接口不依赖 Qt Widgets；网络协议不依赖 Claude；ConPTY 仅在 Windows 平台实现；UI 不允许直接杀本地/远端任意系统进程。

## 5. C++ 核心接口草案

> 下述保留初始接口草案，不是当前可调用 API。M1-02 将错误返回、线程、异步事件和可信工作区上下文细化为明确契约，见 [ADR-013](docs/ADR/013_M1核心接口契约.md)；实施以 core 头文件为准，不再把这些语义留给各实现自行补足。

```cpp
#pragma once
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QVector>

struct LaunchSpec {
    QString executablePath;        // 解析后的受信任可执行路径
    QStringList arguments;         // 参数数组；不拼接不可信 shell 命令
    QString workingDirectory;
    int columns = 120;
    int rows = 32;
};

struct SessionRequest {
    QString name;
    QString providerId;            // "claude", "codex", "shell"
    QString workspaceId;           // 只能引用已登记的可信工作区
    QString worktreeName;          // 可选
};

struct ProviderCapabilities {
    bool interactivePty = true;
    bool nativeBackgroundSession = false;
    bool structuredOutput = false;
    bool canResumeConversation = false;
};

class IProviderAdapter {
public:
    virtual ~IProviderAdapter() = default;
    virtual QString id() const = 0;
    virtual ProviderCapabilities capabilities() const = 0;
    virtual LaunchSpec makeLaunchSpec(const SessionRequest&) const = 0;
};

class ITerminalBackend {
public:
    virtual ~ITerminalBackend() = default;
    virtual bool start(const LaunchSpec& spec) = 0;
    virtual void writeInput(const QByteArray& bytes) = 0;
    virtual void resize(int columns, int rows) = 0;
    virtual void stop() = 0;
    // 输出、退出等事件由实际实现通过回调/信号提供
};
```

辅助接口：

- `ISessionManager`：create/list/get/attach/detach/terminate/subscribe。
- `ITransportClient` / `ITransportServer`：connect/sendControl/sendBinary/reconnect。
- `ISessionRepository`：持久化元数据、状态快照、清理记录。
- `IWorkspaceProvider`：validate/resolve/createWorktree/listWorktrees。
- `IAuthorizationService`：校验客户端身份、连接权限、工作区授权。
- `IEventBus`：结构化业务事件，不依赖终端字符解析。
- `IBuildProvider`：V2 构建请求、输出、状态、取消。

**插件策略**：V1 先静态链接 Provider 并经工厂注册。将来新增动态 DLL 时，不直接公开裸 C++ 虚表跨版本 ABI；优先使用版本化 C ABI 或进程外 IPC。

## 6. Agent 会话与状态机

### 6.1 两套状态必须分开

不要使用单一 `Detached` 枚举表示进程状态，因为一个进程仍在运行但没有附加客户端。

- `processState`：`Creating → Running → Exiting → Exited`，错误可进入 `Failed`。
- `connectionState`：`Attached / Detached`，由当前输入控制权与订阅数量推导。
- `clientState`：`Connecting / Connected / Reconnecting / Offline`。

### 6.2 关键语义

- `session.create`：验证 Provider + Workspace + 并发上限，启动 ConPTY 与 CLI；返回稳定 UUID。
- `session.attach`：对现有会话获取终端输出订阅和（若被授权）输入控制权。
- `session.detach`：取消订阅/释放输入控制权；**不得调用** ClosePseudoConsole。
- `session.terminate`：经用户确认后关闭 ConPTY/进程树，写退出状态。
- `session.resize`：变更 ConPTY 字符列/行。
- `session.list`：仅列出允许访问的会话。
- `session.recover`：Agent 重启后恢复**会话元数据及对话入口**，不是假装恢复原 Win32 进程。

### 6.3 同一会话多个客户端

V1 采用单写者模式：同时允许多个只读观看者（可选），但一个会话最多只有一个拥有输入控制权的客户端。其他客户端申请接管需显式确认/租约到期策略，防止键盘输入相互覆盖。每个会话的 resize 由写者控制。

### 6.4 断线恢复的终端画面真实性

Agent 保留环形输出缓冲、可选的有限本地原始 VT 日志和单调递增的输出序号。只重放“最近几 KB ANSI 字节”不一定重建 Claude 交互式终端的光标及替代屏幕状态。

V1 允许受限的**整会话原始流重放**（设置最大字节数和超限提示），并明确“可能无法精确重建长会话画面”；需另行验证交互 UI。V1.1 可增加 VT 解析器的服务端屏幕快照或经验证的 UI 终端快照机制。不要把「断线附加」错误地实现为「重复启动 Claude」。

### 6.5 Claude 原生后台会话的可选适配

Claude Code 文档现有 `claude --bg`、`claude agents --json` 和 `claude attach <id>` 等后台会话能力（受 CLI 版本/功能条件约束）。这是可以研究的**另一种 Provider 后端能力**，不强行替代通用 ConPTY 机制。若实际版本支持，可单独做 `ClaudeBackgroundProvider`；控制后台会话的行为与普通 PTY Attach 明确区分。

## 7. ConPTY 实现注意事项

1. 创建双向 Pipe 和 `HPCON`；按微软要求使用 `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` + `CreateProcessW`。
2. 输入输出通道分别由工作线程/明确的 I/O 队列处理；避免因同步 Pipe 阻塞造成死锁。微软官方特别警告不要将所有 ConPTY I/O 放在单线程中。
3. 所有 `HANDLE` 用 RAII 包装，严格定义所有权；资源创建失败必须回滚。
4. 终端输出原样保存 UTF-8 和 ANSI 控制序列；不把 VT 当普通文本过滤。
5. `ResizePseudoConsole` 按用户实际列/行调用，做 debounce。
6. 进程结束、用户 Terminate、Agent 退出要区分；避免异常进程树悬挂，可评估 Job Object 但必须测试 CLI 派生进程行为。
7. 处理 `Ctrl+C`、EOF、关闭和排空剩余输出；不得只靠断开 socket 回收进程。
8. `claude` 在 Windows 可能对应不同安装方式和命令包装器（EXE/CMD）。Provider 必须检测真实命令路径并使用安全启动方式，不能对未经校验的目录/命令拼接 `cmd /c`。
9. Worker 输出接入有界缓冲/背压；某个慢客户端不能让整个 Agent 内存增长无上限。

## 8. 通信协议（RADP/1）

**M3-02 当前实现（2026-10-17）**：Agent 已提供显式配置启动的 WebSocket 服务，握手设备令牌摘要认证与会话 ACL 分开，断线仅分离；本机 FakeCli 网络链路已验证。实际启动配置、保活/背压、当前保留会话上限 32 条及远程未验收边界见 [M3-02 说明](docs/M3-02_AgentWebSocket服务.md)。当前无配对 UI/DPAPI/热撤销，不将本机测试视为 Tailscale 双端或真实 Claude 验收。

**M3-01 实施记录（2026-10-17）**：已实现独立协议 DTO 和同步编解码，固定 34 字节二进制头、64 KiB 总帧上限及安全错误，Debug/Release 协议测试通过。下列为初始协议草案；准确消息集合、严格字段规则及当前验证边界见 [M3-01 说明](docs/M3-01_协议编解码.md) 和 [ADR-016](docs/ADR/016_RADP1协议边界.md)。WebSocket、鉴权及路由留给 M3-02，尚未完成远程联调。

通信模型：Qt WebSocket，控制消息 JSON，终端字节流二进制，使用固定版本号。WebSocket 下可使用 `/control` 与 `/terminal/{session_id}` 分别连接；为避免复杂度，实际实现可先同一 socket 双消息类型，但要通过类型和 sessionId 严格区分。

### 8.1 控制消息 Envelope

```json
{
  "version": 1,
  "type": "session.create",
  "request_id": "uuid-request",
  "session_id": null,
  "payload": {
    "name": "Receiver - demapper",
    "provider_id": "claude",
    "workspace_id": "receiver",
    "columns": 120,
    "rows": 32
  }
}
```

响应：

```json
{
  "version": 1,
  "type": "session.created",
  "request_id": "uuid-request",
  "session_id": "uuid-session",
  "payload": {
    "process_state": "running",
    "attachable": true
  }
}
```

统一错误：`error.code / error.message / error.retryable`，不可直接把包含密钥的系统错误原样返回客户端。

### 8.2 建议命令清单

`auth.hello`、`host.info`、`session.create`、`session.list`、`session.attach`、`session.detach`、`session.terminate`、`session.resize`、`session.heartbeat`、`terminal.input`（或二进制帧）、`terminal.output`（二进制帧）、`terminal.overrun`、`workspace.list`、`system.capabilities`。

### 8.3 二进制帧草案

结构可采用：`magic(4) | version(1) | kind(1) | session_uuid(16) | sequence(8 big-endian) | length(4 big-endian) | bytes(N)`，并设置单帧上限（例如 64 KiB）。

约定：

- 输出序号对每个 session 单调递增；Agent 重启后使用新的 session epoch，避免序号重用。
- 重连时客户端可携带 `last_ack_sequence`；Agent 能重放则重放，不能重放则发送 `terminal.overrun` 并明确失去精确画面同步。
- `heartbeat`、身份过期、重试退避、最大消息大小、数据队列上限必须实现。
- JSON 规范和二进制协议放到 `protocol/`，提供跨版本兼容性测试。

## 9. Workspace 与 Git Worktree

- 用户先在家庭 Agent 登记允许操作的工程根目录，例如 `D:\Projects`；仅可选已登记的项目 ID，不允许远端客户端输入任意绝对目录随意执行。
- 路径规范化：校验绝对路径、软链接/重解析点、盘符/UNC、越界 `..` 与访问权限，确保最终解析路径属于授权范围。
- 同项目多 AI 会话默认启用独立 worktree 的能力预留。V1 可先要求人工预建 worktree 并提供提示；V1.1 自动创建。不同会话不可误删彼此工作树。
- 对话与代码更改分开；程序不得自动 `git add . && git commit`，除非用户触发明确操作且可审查 diff。
- NAS Gitea 负责版本历史，家庭 Agent 不复制 Claude 密钥到 NAS；公司编译机可使用 Gitea 只读 Deploy Key。

## 10. 数据结构与持久化

SQLite 推荐表：

- `hosts`（客户端保存）：`id`, `display_name`, `tailscale_host`, `port`, `last_seen`。
- `projects`（Agent）：`id`, `name`, `root_path`, `repo_url`, `allowed_provider_ids`。
- `sessions`：`id`, `name`, `provider_id`, `project_id`, `workspace_path`, `created_at`, `last_seen`, `process_state`, `exit_code`, `epoch`。
- `session_events`：`id`, `session_id`, `event_type`, `timestamp`, `public_payload`。
- `settings`：`key`, `value`, `schema_version`。

禁止明文存储 Claude 的登录 token、SOCKS5 密码、NAS SSH 私钥。Agent/Client 配对凭据使用 DPAPI / Windows Credential Manager；日志脱敏，默认不开完整终端录制（可能包含用户输入的密码与隐私资料）。

## 11. 安全设计（MVP 验收硬要求）

1. **主机访问**：仅允许公司授权设备在 Tailscale 内访问 Agent TCP 端口；使用 Tailscale Grants 限制设备/端口。
2. **绑定地址**：Agent 默认 `127.0.0.1`，启用远程时显式绑定 Tailscale 地址；不默认监听 `0.0.0.0`。
3. **应用身份认证**：第一次配对发放不可猜测、可撤销的设备令牌（或公钥挑战方式）。身份认证不依赖“在 Tailnet 就天然可信”。
4. **权限最小化**：V1 单个授权开发者、工作区白名单、单写者控制、敏感操作确认；不以管理员/LocalSystem 身份执行 CLI。
5. **协议保护**：限制消息长度、频率和并发；校验每个 sessionId 的权限及输入长度；拒绝未知协议版本。
6. **凭据保管**：Claude Code 认证只保留家里机器；客户端不能读取 Agent 环境变量或密钥文件。
7. **安全执行**：不接受任意远程进程启动命令；只允许经过注册的 Provider + 固定启动模板（普通 Shell Provider 需另行审查）。
8. **可审计**：创建、附加、终止、认证失败、授权失败记录操作事件；日志不记录完整凭据或敏感终端字节。
9. **更新**：Agent 与 Client 版本协商；升级失败可回滚；重要更改有离线安装包和恢复配置机制。

## 12. UI 信息架构

**已选视觉基线**：采用深灰底色与青绿色强调色的紧凑桌面工具布局，详见 [05_RADManager_UI设计规格.md](05_RADManager_UI设计规格.md)。首版实现内置深色主题，集中管理 Qt 控件、图标与 xterm.js 的主题参数，预留主题标识和配置保存；后续换肤不重建或中断会话。参考图为概念稿，尚未经过真实 Qt 运行验收。

```text
主窗口
 ├─ 顶部：连接到 Home Agent / Tailscale 状态 / 全局操作
 ├─ 左侧：项目树、工作区、会话列表（可折叠）
 ├─ 中部：多个终端标签（一个 Session ID 对应一页）
 │     └─ xterm.js：终端输出、选中文字、复制粘贴、搜索、窗口尺寸
 ├─ 右侧（预留）：Git Diff / Task / Build / Provider 状态
 └─ 底部：Agent 健康、网络时延、错误消息及通知
```

建议交互：新建会话对话框只需「项目 / Provider / 会话名称 / 工作区」；标签页关闭时弹出“分离”与“终止”两种明确选择，默认是**分离**。重要状态使用文本+图标（不是只看颜色）。

终端资源通过 Qt Resource 嵌入，使用 QWebEngineView 加载本地前端；Qt/C++ 与 xterm.js 由 QWebChannel 桥接，网络传输由 C++ 客户端控制层处理。不要让不可信网页直接获得整个 Agent 控制 API。

## 13. 工程目录结构

```text
RADManager/
├─ AGENTS.md                       # Codex 开发行为规则
├─ README.md
├─ CMakeLists.txt
├─ CMakePresets.json
├─ docs/
│  ├─ ARCHITECTURE.md
│  ├─ PROTOCOL_V1.md
│  ├─ SECURITY.md
│  ├─ TEST_PLAN.md
│  └─ ADR/                       # 架构决策记录
├─ apps/
│  ├─ RemoteClient/
│  │  ├─ app/
│  │  ├─ ui/
│  │  ├─ controllers/
│  │  └─ resources/terminal/     # 打包后的 xterm.js 前端
│  └─ RemoteAgent/
│     ├─ bootstrap/
│     ├─ server/
│     └─ diagnostics/
├─ core/
│  ├─ protocol/
│  ├─ model/
│  ├─ session/
│  ├─ terminal/
│  ├─ provider/
│  ├─ workspace/
│  ├─ transport/
│  ├─ storage/
│  ├─ security/
│  └─ events/
├─ platforms/windows/
│  ├─ ConPty/
│  ├─ CredentialStore/
│  └─ Process/
├─ providers/
│  ├─ Claude/
│  ├─ Shell/
│  └─ Codex/                    # 后续实现
├─ extensions/
│  ├─ Git/
│  ├─ Build/
│  └─ Notifications/
├─ frontend/terminal/            # xterm.js 源代码、package lock、构建脚本
├─ tests/
│  ├─ unit/
│  ├─ protocol/
│  ├─ integration/
│  ├─ e2e/
│  └─ fixtures/FakeCli/
└─ tools/
   └─ packaging/
```

核心原则：Qt UI 层不直接包含 Windows.h；平台层不能依赖 Claude 专有细节；协议定义与服务端实现分离。

## 14. 交付阶段 / 任务拆分

> 粗略安排按依赖顺序，不是承诺完成天数。每阶段必须可编译、可测试、可回滚。详细开发卡片见 `03_CODEX_分阶段任务清单.md`。

| 里程碑 | 交付重点 | 验收门槛 |
|---|---|---|
| M0：技术预研 | ConPTY 单进程 POC，Qt 5.15.2 + VS 2022/C++20 工具链验证，xterm.js 字节传输，家中连通性验证 | 本机编译、moc、链接和启动通过；中文/ANSI、快捷键、resize、复制粘贴、高吞吐输出正常；锁定前端版本；确认网络路线 |
| M1：工程骨架 | CMake、核心接口、Client/Agent 两程序、Qt Test、最小部署检查 | 本机及配置指定工具链的干净环境能构建和执行测试；部署目录可启动，WebEngine 辅助进程/资源、Qt 插件和 SQLite 驱动完整 |
| M2：本地 Agent | SessionManager、ConPTY、FakeCLI、多实例 | 3 个并发会话互不影响，关闭客户端不杀进程 |
| M3：远程协议 | 鉴权、WebSocket、二进制 I/O、错误处理 | Tailscale 内 Client 可以可靠创建/附加/终止 |
| M4：Qt 多标签终端 | QTabWidget + WebEngine + xterm.js + QWebChannel | 终端可交互，中文、热键、resize、复制正常 |
| M5：重连和持久化 | Session ID、状态恢复、心跳、日志/缓冲 | 断网重连重新附加；Agent 重启诚实显示进程丢失 |
| M6：Claude Provider | 工具探测、启动规则、项目选择、权限、图片附件与独立预览 | 本机先验证真实交互及图片引用，再从公司远程启动 2-3 个 Claude 会话并验证附件传输 |
| M7：安全与稳定 | ACL/Grants、密钥、授权工作区、资源配额、异常注入 | 测试用例通过、无公网暴露、无日志泄密 |
| M8：发布版 | 打包、配置迁移、部署手册、兼容测试 | 可在家/公司部署并回滚 |
| M9（可选 V1.1） | Git Worktree、Codex Provider、原生 Claude 后台会话 | 不修改 core 公共接口即可扩展 |

M4-01 实施记录（2026-10-17）：Qt Client 基础界面已实现主机状态、工作区会话树、多标签和安全确认，并以 FakeAgent 完成界面验收；Debug/Release 构建通过、Release 全套 25/25 测试通过。主题集中且保存 dark 标识，为后续换肤预留；终端字节流及真实 Agent 连接仍属 M4-02，整个 M4 尚未完成。验证范围见 [M4-01 说明](docs/M4-01_QtClient基础界面.md)。

当前实现记录（2026-10-17）：M2-01 Windows 后端与 M2-02 SessionManager 已实现。真实三会话验证全部 detach 后继续运行、同 ID 重附加、显式终止及输入隔离；Release 全套 20/20 通过，详见 [M2-02 说明](docs/M2-02_SessionManager.md)。用户已批准提交 M2 并继续后续任务，产品客户端关闭/网络断线的端到端验证仍在后续任务，不把本地 detach 测试视为远程验收。

## 15. 测试矩阵

### 本机工具链与部署测试

- 在本机 Windows 10 IoT 企业版 LTSC x64（19044）使用 Qt 5.15.2 MSVC x64、VS 2022 / v143、C++20 和 CMake，从新构建目录配置、编译、链接并执行 CTest。
- 在 VS 2022 中调试 Client/Agent，确认启动及断点可用；分别验证 Debug 与 Release 配置，不混用 Qt DLL 和插件。
- M0 的 Qt 终端 POC 验证中文输入法、ANSI/VT、Ctrl+C、方向键、复制粘贴、resize、全屏重绘及高吞吐输出，记录实际采用的 xterm.js/FitAddon 版本。
- M1 从独立部署目录启动最小 Client/Agent；通过最小冒烟检查确认 WebEngine 页面能加载、辅助进程和资源/locales 可用、平台插件可加载、QSQLITE 驱动能打开临时数据库。不提前实现真实终端或业务持久化。
- 部署检查不能依赖开发机全局 Qt PATH 掩盖缺失依赖；M8 继续在目标机器做完整安装验收。Windows 11 后续单独记录兼容测试结果。

### 单元测试

- Provider registry 加载与能力查询。
- 正确与错误路径的 workspace 白名单（含 `..`、重解析点）。
- Session state/transitions，重复 attach/detach/terminate 幂等性。
- Protocol JSON 校验、未知版本、过大包、无效 UUID。
- 权限拒绝及退出码映射。

### 集成测试

- `FakeCli` 输出 ANSI 颜色、Unicode 中文、频繁大量日志和全屏重绘。
- 一次运行 3-5 个 FakeCli，各自输入与输出隔离。
- 一条终端快速产生日志，另一条终端操作不卡顿。
- ConPTY 输出/输入读写线程退出、错误路径和句柄泄漏。
- 网络断开/恢复后的重订阅以及日志回放边界。

### E2E

- 家庭 Agent 在线，公司客户端登录、打开 3 标签、输入指令、关闭客户端、重新打开继续操作原进程。
- 选择不同项目目录、处理不同 worktree，不串目录。
- 家庭软路由线路故障时，客户端显示离线，不误判为已终止会话。
- 家里仍可通过本地网络向 NAS 推送，公司直接经 Tailscale 从 NAS 拉取。
- Agent 意外结束后，UI 显示进程不可附加，提供明确“恢复对话/新建会话”操作，不伪造成功恢复。

### 安全测试

- 未配对电脑连接拒绝；过期 token 拒绝；会话跨授权工作区拒绝；端口只能在 Tailnet 可达；公网扫描无 Agent 开放端口。
- 输入包超过限制被拒绝；日志中不出现认证 token；客户端无法执行任意系统命令接口。

## 16. Codex 开发协作流程

采用“你设计核心架构，Codex 分模块实现”的方式：

1. 先将本开发计划、`AGENTS.md` 和任务清单放进新仓库；Codex 读取三份文档。
2. 每次仅给 Codex **一张明确任务卡**：输入、接口、允许改动范围、验收标准。
3. 要求 Codex 在改动前说明要修改的文件和原因；只实现当前里程碑，不提前做 UI 美化或扩展功能。
4. 每个 PR / 阶段编译并运行测试；Codex 输出构建命令、测试结果、修改清单、风险与未实现项。
5. 你检查 SessionManager/ConPTY/Protocol 核心架构和异常路径，再决定合并；避免 AI 一次把全部模块堆成一体。
6. 稳定后再提交到 NAS Gitea；重要里程碑打 Git tag。

**Codex 首轮指令模板**：

> 阅读 `01_RADManager_完整开发计划.md`、`AGENTS.md`、`03_CODEX_分阶段任务清单.md`，先确认配套文档已同步本机技术基线。使用 Qt 5.15.2 MSVC x64、VS 2022 / v143、C++20 和 CMake，在本机 Windows 10 IoT 企业版 LTSC x64（19044）验证。目前只执行 TASK-M0-01：建立最小 ConPTY + FakeCLI 技术验证，输出可运行示例、测试、构建说明，本任务仍不接入 Qt。不得实现其他模块。先提出 POC 方案、影响文件和验收方法，再开始编码。重点验证 UTF-8/VT、输入输出死锁和窗口尺寸变化；Qt 工具链及终端前端验证在后续 TASK-M0-02 完成。

## 17. 关键决策记录与待验证事项

| 编号 | 决策 / 验证事项 | 默认做法 |
|---|---|---|
| ADR-001 | 客户端 / Agent 拆分 | 两独立 EXE，协议版本化 |
| ADR-002 | 终端底层 | Windows 原生 ConPTY，而非文本 IO 模拟 |
| ADR-003 | Agent 运行身份 | 普通 Windows 登录用户；任务计划程序“登录时启动” |
| ADR-004 | UI 渲染 | Qt 5.15.2 Widgets + WebEngine + WebChannel，本地嵌入经本机 POC 验证并锁定版本的 xterm.js/FitAddon |
| ADR-005 | 可扩展性 | 静态 Provider 注册 + 稳定接口；先不动态 DLL |
| ADR-006 | 安全边界 | Tailnet Grants + app pairing + 项目白名单 |
| ADR-007 | 多会话控制权 | 单写者，断线释放租约 |
| ADR-008 | Git 并行修改 | 推荐 worktree，禁止默认共享写入目录 |
| ADR-009 | 网络先决条件 | 先实测家中笔记本 Tailscale；不得默认能透过 SOCKS5 链路 |
| ADR-010 | CLI 后台原生能力 | 作为能力开关单独适配，不能和 ConPTY Attach 混同 |
| ADR-011 | 本机开发基线 | Qt 5.15.2 MSVC x64 + VS 2022 / v143 + C++20 + CMake；Windows 10 IoT 企业版 LTSC（19044）首验，Windows 11 后续兼容测试 |
| ADR-012 | 工具链与部署证据 | M0 验证编译/链接/启动及前端兼容，M1 验证最小部署；依赖已安装不视为项目测试通过 |
| ADR-013 | 核心接口契约 | 控制线程非阻塞请求、明确错误与最终事件、可信工作区上下文、有界终端字节流；详见 docs/ADR/013_M1核心接口契约.md |

## 18. 第一版验收定义（Definition of Done）

- [ ] 本机 Windows 10 IoT 企业版 LTSC x64（19044）能使用 Qt 5.15.2 MSVC x64 + VS 2022 / v143 + C++20 + CMake 从源码构建、调试、运行 Client/Agent，并执行测试。
- [ ] 家庭 Agent 和公司 Qt Client 均在目标设备完成部署运行验证，WebEngine 辅助进程、资源、Qt 插件及 SQLite 驱动无缺失；记录 Debug/Release 验证结果。
- [ ] xterm.js 与 FitAddon 已通过本机兼容验证并锁定版本，终端资源完全离线加载；安装检查与实际测试结果分别记录。
- [ ] 在公司 Client 中同时打开 3 个独立的远程 CLI 标签。
- [ ] 终端能正确显示 ANSI、中文、交互式提示以及窗口尺寸变化。
- [ ] 真实 Claude CLI 的 `/` 菜单、Tab 补全、Esc 和方向键操作正常；图片附件可预览、受控传输及引用，错误明确可见，不以本地路径冒充远端文件。
- [ ] 关闭一个标签选择“分离”，该 CLI 仍运行；重连能附加到同一进程。
- [ ] 终止一个会话，不影响其他会话。
- [ ] 网络断开后，不发生未授权普通 WAN 回退；网络恢复能恢复 Client 通信。
- [ ] 未获授权客户端无法使用 Agent；源代码和登录状态留在家庭端。
- [ ] 同仓库并行工作时不同 worktree 的修改互不覆盖。
- [ ] Agent 意外退出或系统重启的限制在 UI 中准确呈现。
- [ ] 后续 NAS 部署后，通过 NAS Gitea / 公司编译链路完成真实的 Qt/C++ 工程演示；该项暂缓，不阻塞家庭测试机远程 CLI 联调。
- [ ] 日志轮转、协议兼容、错误处理与用户文档完成。

## 19. 官方技术参考

- Microsoft ConPTY 创建会话：https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session
- Microsoft ConPTY API：https://github.com/MicrosoftDocs/Console-Docs/blob/main/docs/createpseudoconsole.md
- Qt 5.15 文档归档（API 以本机 5.15.2 头文件与文档为准）：https://doc.qt.io/archives/qt-5.15/
- Qt WebEngine / Chromium 版本：https://wiki.qt.io/QtWebEngine/ChromiumVersions
- Microsoft MSVC 二进制兼容条件：https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017
- Windows 10 IoT Enterprise LTSC 2021 生命周期：https://learn.microsoft.com/en-us/lifecycle/products/windows-10-iot-enterprise-ltsc-2021
- xterm.js 浏览器支持：https://github.com/xtermjs/xterm.js#browser-support
- xterm.js：https://xtermjs.org/docs/guides/using-addons/
- Claude Code CLI：https://code.claude.com/docs/en/cli-reference
- Tailscale Grants：https://tailscale.com/docs/features/access-control/grants

---

**架构负责人最终原则**：先保证“稳定多 CLI + 准确会话语义 + 安全远程连接”，再扩展多 Provider、Git、CI、任务调度。所有新增功能只依赖明确接口，不绕过 SessionManager 和授权层。
