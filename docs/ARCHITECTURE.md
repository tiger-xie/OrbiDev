# RADManager 当前工程边界

## M1-01 已建立的目标

| 目标 | 路径 | 依赖及职责 |
|---|---|---|
| RADCore | core/model | QtCore；报告编译基线，不包含业务模型或 UI |
| RADProtocol | core/protocol | 协议主版本常量；不包含编解码、网络或 Qt GUI |
| RemoteClient | apps/RemoteClient | RADCore、RADProtocol、Qt Widgets/WebEngineWidgets；最小本地启动页和部署烟测 |
| RemoteAgent | apps/RemoteAgent | RADCore、RADProtocol、QtCore/Sql；启动说明及 SQLite 内存库烟测，不监听网络 |
| RADTests | tests/unit | Qt Test；验证加载的 Qt 版本、位数、语言标准及库目标可链接 |
| M0 POC | tools/TerminalPoc、platforms/windows/ConPty、tests/fixtures | 独立保留技术验证，不被产品 Client/Agent 链接 |

根 CMake 的 RAD_BUILD_APPLICATIONS、RAD_BUILD_CONPTY_POC、RAD_BUILD_TERMINAL_POC 分别控制产品骨架及两个 POC，默认均开启。关闭全部 POC 后，产品骨架无需下载 ConPTY 运行库。关闭产品骨架和终端 POC 后，M0-01 仍可不依赖 Qt 构建。

依赖方向为 apps → core / protocol。Agent 不依赖 Widgets、WebEngine、WebChannel；Client 不直接包含 Win32 或 Provider 实现。M1-02 再定义会话、后端和 Provider 的模型/抽象接口，不用无行为 Mock 提前占据接口。

## 本轮计划调整

用户确认当前电脑为公司端，明确将 M0-03 网络验证延期，允许继续本地开发。网络状态仍为未验收；后续 Claude 终端可以先在本机验证。本轮不启动 Claude、不接入登录凭据或真实项目、不修改网络配置。

## 运行与错误边界

- Client 只显示“工程骨架，未连接Agent”，无终端/会话业务；WebEngine 页面为内嵌常量内容，CSP 不允许外部资源。
- Agent 默认说明当前未监听后退出；不是已完成的常驻 Agent。`--smoke-test` 才创建 SQLite 内存数据库，不写业务数据文件。
- 两个程序的 `--smoke-test` 必须通过真实运行检查后返回 0；失败返回非零，Client 有超时限制。
- M0 的 detach、持久化、鉴权等约束不因建立工程骨架而改变。

构建、部署和验证记录见 [M1-01 工程骨架](M1-01_工程骨架与验证.md)。
