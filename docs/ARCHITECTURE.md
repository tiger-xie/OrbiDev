# RADManager 当前工程边界

## M1-01 已建立的目标

| 目标 | 路径 | 依赖及职责 |
|---|---|---|
| RADCore | core/model、core/terminal、core/provider、core/session | QtCore；编译基线、会话值模型、错误结果及抽象接口，不包含平台实现或 UI |
| RADProtocol | core/protocol | 协议主版本常量；不包含编解码、网络或 Qt GUI |
| RemoteClient | apps/RemoteClient | RADCore、RADProtocol、Qt Widgets/WebEngineWidgets；最小本地启动页和部署烟测 |
| RemoteAgent | apps/RemoteAgent | RADCore、RADProtocol、QtCore/Sql；启动说明及 SQLite 内存库烟测，不监听网络 |
| RADTests | tests/unit | Qt Test；验证加载的 Qt 版本、位数、语言标准及库目标可链接 |
| RADSessionContractTests | tests/unit | Qt Test；值校验及内存替身的接口契约，不代表生产会话实现 |
| M0 POC | tools/TerminalPoc、platforms/windows/ConPty、tests/fixtures | 独立保留技术验证，不被产品 Client/Agent 链接 |

根 CMake 的 RAD_BUILD_APPLICATIONS、RAD_BUILD_CONPTY_POC、RAD_BUILD_TERMINAL_POC 分别控制产品骨架及两个 POC，默认均开启。关闭全部 POC 后，产品骨架无需下载 ConPTY 运行库。关闭产品骨架和终端 POC 后，M0-01 仍可不依赖 Qt 构建。

依赖方向为 apps → core / protocol。Agent 不依赖 Widgets、WebEngine、WebChannel；Client 不直接包含 Win32 或 Provider 实现。M1-02 已定义会话、后端和 Provider 的模型/抽象接口，尚未接入应用入口。

## M1-02 接口契约

核心接口使用 QtCore。IProviderAdapter 同步转换可信工作区和请求为 LaunchSpec，不执行 IO；ITerminalBackend 与 ISessionManager 使用抽象 QObject，控制方法在对象所属 Agent 控制线程调用，接受与最终完成分开。输出通过合并通知和有界拉取交付，原始字节不解码；截断显式报告。SessionStatus 仅表示进程状态，附加人数与写者独立，分离不终止进程。Creating 阶段的创建主体可显式取消。

错误结果、事件顺序及生命周期见各公开头文件和 [ADR-013](ADR/013_M1核心接口契约.md)，调用链和测试范围见 [M1-02 核心模型与接口](M1-02_核心模型与接口.md)。生产线程与资源回收由 M2 实现和验证；M1 的内存替身不承担生产行为。

## 本轮计划调整

用户确认当前电脑为公司端，并已准备家庭测试电脑，Claude CLI 可用、Tailscale 已连接（用户提供证据，非代理跨端验证）。后续优先在该机联调远程 CLI；NAS 不参与终端链路，保留未来 Git 托管规划。正式出口/Fail Closed 与 NAS 验收延期，双端路径和 Agent 端口仍待验证。项目按原里程碑继续开发，本轮仅调整文档。

## 运行与错误边界

- Client 只显示“工程骨架，未连接Agent”，无终端/会话业务；WebEngine 页面为内嵌常量内容，CSP 不允许外部资源。
- Agent 默认说明当前未监听后退出；不是已完成的常驻 Agent。`--smoke-test` 才创建 SQLite 内存数据库，不写业务数据文件。
- 两个程序的 `--smoke-test` 必须通过真实运行检查后返回 0；失败返回非零，Client 有超时限制。
- M0 的 detach、持久化、鉴权等约束不因建立工程骨架而改变。

构建、部署和验证记录见 [M1-01 工程骨架](M1-01_工程骨架与验证.md)。
