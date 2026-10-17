# RADManager · 开发计划资料包

## 当前可运行原型

M0-02 已实现本地离线终端演示，运行本机构建目录 `build-terminal-deploy/TerminalPoc.exe`。构建、自测及人工验收步骤见 [Qt 终端技术验证](docs/M0-02_Qt终端技术验证.md)。这是本地模拟终端，不执行系统命令，也不连接远程 Agent；用户已确认 M0-02 验收通过，具体记录见验证说明。

## 当前工程骨架

M1-01 已建立 Client/Agent、Core/Protocol 和 Qt Test 目标，本机 Debug/Release 构建及部署烟测通过。构建、运行、调试入口及已知限制见 [工程骨架与验证](docs/M1-01_工程骨架与验证.md)。M0-03 已有家庭测试电脑（用户确认 Claude CLI 可用、Tailscale 已连接），双端联调待实施；NAS 与正式网络场景延期。真实 Claude 尚未接入；本机 FakeCli 会话服务已由 M3-02 的 --config 入口提供。

## 文件使用顺序

M1-02 已定义核心模型、Provider/终端/会话管理接口，测试入口及生产实现边界见 [核心模型与接口](docs/M1-02_核心模型与接口.md)。应用界面仍是 M1-01 骨架；真实 Claude 和附件功能尚未接入；FakeCli 服务见下方 M3-02。

- `01_RADManager_完整开发计划.md`：总规格；先由项目负责人审核，明确范围和架构。
- `AGENTS.md`：放到 Codex 直接工作的 Git 仓库**根目录**，作为持续有效的工程规则。
- `03_CODEX_分阶段任务清单.md`：按任务卡逐项交给 Codex。
- `04_给Codex的首次启动指令.md`：复制给 Codex，先实施 M0 技术原型，别一次生成大项目。
- `02_AGENTS.md`：`AGENTS.md` 的同内容备份；不需要两份都保存在最终源码仓库。

## 操作建议

1. 在自己的工作目录创建 `RADManager` Git 仓库。
2. 复制主要计划、`AGENTS.md`、任务清单到仓库根目录。
3. 把计划中的路径和 Qt/MSVC 版本调整成自己的实际环境。
4. 使用 `04_给Codex的首次启动指令.md` 发起首个任务。
5. 审核 M0 的 ConPTY 输入输出、死锁与进程生命周期后，再继续到 M1。
6. 每完成一个里程碑，先审代码、运行测试，再提交至 NAS Gitea。

> 安全说明：计划中均为示例地址，不包含任何真实访问令牌、代理密码或银行卡信息；不要把实际凭据纳入版本库。

## M2-01 后端实现

Windows ConPTY 后端与真实 FakeCli 集成测试见 [M2-01 说明](docs/M2-01_Windows终端后端.md)。Agent 已在 M3-02 组合真实 FakeCli 会话；Client 界面仍待后续任务。

## M2-02 本地会话管理

SessionManager 提供创建、查询、附加/分离、控制权租约和显式终止，测试入口见 [M2-02 说明](docs/M2-02_SessionManager.md)。已接入真实后端的集成测试；网络入口已由 M3-02 提供；产品界面仍待后续任务。

## M3-01 协议编解码

RADP/1 提供控制消息、终端二进制帧、版本协商及错误协议，字段与测试入口见 [M3-01 说明](docs/M3-01_协议编解码.md)。M3-02 已实现本机服务与鉴权路由；家庭 Agent 的跨端连接仍待实测。

## M3-02 Agent WebSocket 服务

使用 --config 加载本地设备摘要、工作区 ACL 与固定 FakeCli 配置，启动经鉴权的常驻 Agent。配置、协议步骤及本机验收见 [M3-02 说明](docs/M3-02_AgentWebSocket服务.md)。默认无配置仍不监听；不自动配置 Tailscale/防火墙，不包含真实 Claude 或远程 UI。
