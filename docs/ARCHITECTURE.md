# RADManager 当前工程边界

## M1-01 已建立的目标

| 目标 | 路径 | 依赖及职责 |
|---|---|---|
| RADCore | core/model、core/terminal、core/provider、core/session | QtCore；编译基线、会话值模型、错误结果、抽象接口及 SessionManager，不包含平台实现或 UI |
| RADProtocol | core/protocol | QtCore；RADP/1 DTO、JSON/二进制编解码与校验；不包含网络、会话实现或 Qt GUI |
| RemoteClient | apps/RemoteClient | RADClientUi、RADCore、RADProtocol、Qt Widgets/WebEngineWidgets；基础界面及独立 WebEngine 烟测 |
| RemoteAgent | apps/RemoteAgent | 配置与组装；--config 启动 RADAgentTransport、SessionManager、固定 FakeCli Provider 和 Windows 后端；保留 SQLite 烟测 |
| RADTests | tests/unit | Qt Test；验证加载的 Qt 版本、位数、语言标准及库目标可链接 |
| RADSessionContractTests | tests/unit | Qt Test；值校验及内存替身的接口契约，不代表生产会话实现 |
| M0 POC | tools/TerminalPoc、platforms/windows/ConPty、tests/fixtures | 独立保留技术验证，不被产品 Client/Agent 链接 |

根 CMake 的 RAD_BUILD_APPLICATIONS、RAD_BUILD_CONPTY_POC、RAD_BUILD_TERMINAL_POC 分别控制产品骨架及两个 POC，默认均开启。M2 新增 RAD_BUILD_WINDOWS_BACKEND，默认跟随产品骨架开启；仅构建不含后端的骨架时还需关闭此开关，才无需下载 ConPTY 运行库。关闭产品骨架、Windows 后端和终端 POC 后，M0-01 仍可不依赖 Qt 构建。

依赖方向为 apps → core / protocol。Agent 不依赖 Widgets、WebEngine、WebChannel；Client 不直接包含 Win32 或 Provider 实现。M1-02 定义会话、后端和 Provider 抽象；M3-02 已通过 Agent --config 入口组合真实后端和固定 FakeCli。

## M1-02 接口契约

核心接口使用 QtCore。IProviderAdapter 同步转换可信工作区和请求为 LaunchSpec，不执行 IO；ITerminalBackend 与 ISessionManager 使用抽象 QObject，控制方法在对象所属 Agent 控制线程调用，接受与最终完成分开。输出通过合并通知和有界拉取交付，原始字节不解码；截断显式报告。SessionStatus 仅表示进程状态，附加人数与写者独立，分离不终止进程。Creating 阶段的创建主体可显式取消。

错误结果、事件顺序及生命周期见各公开头文件和 [ADR-013](ADR/013_M1核心接口契约.md)，调用链和测试范围见 [M1-02 核心模型与接口](M1-02_核心模型与接口.md)。生产线程与资源回收由 M2 实现和验证；M1 的内存替身不承担生产行为。

## 本轮计划调整

用户确认当前电脑为公司端，并已准备家庭测试电脑，Claude CLI 可用、Tailscale 已连接（用户提供证据，非代理跨端验证）。后续优先在该机联调远程 CLI；NAS 不参与终端链路，保留未来 Git 托管规划。正式出口/Fail Closed 与 NAS 验收延期，双端路径和 Agent 端口仍待验证。项目按原里程碑继续开发，此前已提交网络规划文档；当前实现范围已扩展至 M4-01 FakeAgent 客户端界面；跨端网络仍待实测。

## 运行与错误边界

- Client 默认显示原生基础界面，以内存 FakeAgent 验证会话操作；无网络或终端交互。--smoke-test 单独打开原 WebEngine 内嵌页面，CSP 不允许外部资源。
- Agent 默认说明当前未监听后退出；显式 --config 才进入经鉴权的常驻服务。`--smoke-test` 才创建 SQLite 内存数据库，不写业务数据文件。
- 两个程序的 `--smoke-test` 必须通过真实运行检查后返回 0；失败返回非零，Client 有超时限制。
- M0 的 detach、持久化、鉴权等约束不因建立工程骨架而改变。

构建、部署和验证记录见 [M1-01 工程骨架](M1-01_工程骨架与验证.md)。

## M2-01 Windows 终端后端

RADWindowsTerminal 实现 ITerminalBackend，依赖 RADCore 与纯 Win32 的 RADConPtyProcess；原 RADConPtyPoc 复用同一引擎。M2-02 首先通过独立测试组合，M3-02 的 Agent 服务入口已链接此后端。

每个会话拥有生命周期线程及管道读写线程，控制线程只提交请求与消费合并通知。输出有界且显式报告丢弃，finished 在关闭与尾部输出归并后发出。白名单是启动目录授权，不是进程文件系统沙箱。

源码链路、配置与验收见 [M2-01 Windows 后端](M2-01_Windows终端后端.md) 与 [ADR-014](ADR/014_Windows终端后端.md)。

## M2-02 本地会话管理

SessionManager 位于 core/session，实现 ISessionManager，只依赖 QtCore 与 Provider/后端抽象。可信构造配置提供静态 Provider、工作区 ACL 及后端工厂；平台类型仅在组合方与集成测试出现。无需新增层级、依赖或网络接口。

管理器独占后端，finished 前归并尾部，随后释放后端，保留有限元数据与历史。detach 只解除订阅和写权；默认 30 秒单写者租约到期降为只读，输入、resize 和原写者重复附加可续租。每订阅独立游标、合并通知和截断报告；活跃会话与终态保留数量分别限制。

静态 ACL 只承担工作区授权；M3-02 的设备令牌摘要承担网络身份认证。详见 [SessionManager 流程与验收](M2-02_SessionManager.md) 和 [ADR-015](ADR/015_SessionManager与控制权租约.md)。

## M3-01 协议边界

RADProtocol 使用独立协议 DTO 与同步无状态编解码函数，仅依赖 QtCore，不依赖 RADCore。JSON 和二进制消息上限均为 64 KiB，收发双向验证；固定公开错误不透传系统详情。语法解码、版本协商、会话存在性与授权分开，不将解析成功视为认证成功。

M3-02 的服务层已调用协议库执行鉴权后的会话路由。字段与源码入口见 [M3-01 说明](M3-01_协议编解码.md) 和 [ADR-016](ADR/016_RADP1协议边界.md)。

## M3-02 Agent 服务

RADAgentTransport 位于 core/transport，依赖 RADCore、RADProtocol、Qt Network/WebSockets；借用同线程 ISessionManager。RADFakeCliProvider 位于 providers/FakeCli，固定使用本地可信配置中的 EXE，不接收远程任意启动命令。Agent 负责配置、生命周期与组装，业务路由不写进入口。

握手 Authorization 令牌匹配静态 SHA-256 摘要后确定 clientId，hello 只做版本协商。每身份一个连接，每次会话操作继续验证 ACL；断线/超时/背压只 detach。输入按每订阅帧序号拒绝重放，输出采用绝对字节偏移；重连有限历史重放不等于精确画面恢复。

默认不监听，配置后只允许本机 IPv4 回环或本机 Tailscale 地址范围；不修改系统网络配置，不新增依赖。配对 UI、DPAPI、持久化日志、真正跨端验证留给后续阶段。配置与验收见 [M3-02 说明](M3-02_AgentWebSocket服务.md)。

## M4-01 客户端基础界面

RADClientUi 位于 apps/RemoteClient，包含 ClientWindow、ClientController、FakeAgent 与集中 Theme；只依赖 Qt Widgets。UI 操作由协调器串行提交，按会话 ID 去重；分离成功才移除标签，关闭窗口仅断开。FakeAgent 为验收替身，不承担身份认证或生产会话管理。真实协议接入留给 M4-02，详见 [M4-01 说明](M4-01_QtClient基础界面.md)。
