# OrbiDev · SSH 工作台

当前开发方向为 Qt 内嵌 SSH 工作台，通过 Tailscale 连接家庭电脑上的 Claude。原 RADManager Client/Agent 暂停开发，代码与历史资料保留。关闭客户端或断线不保证远程任务继续运行。

## 当前启动入口

- 本机开发版：构建 Release 后双击根目录 **Start-OrbiDev.cmd**；原 Start-SshWorkbench.cmd 保留兼容。
- 候选便携包：解压后双击包内 **Start-OrbiDev.cmd**。首次使用通过“连接设置”运行配置向导。
- 家庭电脑只需使用包内 **Start-HomeSetup.cmd** 做一次公钥授权；日常连接不要求家庭配置工具常驻。
- 当前版本为 **0.1.0-preview**，属于候选验证版。版本可在标题栏、`--version` 或工作台“连接设置 → 关于 SSH 工作台”查看。

使用与迁移见 [SSH 工作台说明](docs/SSH内嵌工作台_使用与验证说明.md)，候选包与未完成验收见 [发布与升级说明](docs/SSH工作台_发布与升级说明.md)。Windows 11、真实双端稳定性及既有 Qt／剪贴板验证缺口尚未全部关闭，不按正式发布宣传。

以下为暂停的 Client/Agent 历史资料，不是当前 SSH 工作台的启动流程。

## 历史 Client/Agent 启动（暂停）

编译 Debug 后，在仓库根目录先双击 **[Start-RemoteAgent.cmd](Start-RemoteAgent.cmd)**，保留窗口，再双击 **[Start-RemoteClient.cmd](Start-RemoteClient.cmd)**。Client 打开后点击“连接”→“新建会话”，即可验证真实 FakeCli 终端。脚本自动处理 Qt 路径、联调配置与设备令牌，无需手填参数。Release 启动和详细步骤见 [启动说明](docs/M4-02_远程终端集成.md#推荐本机双击启动)。

## 当前可运行原型

M0-02 已实现本地离线终端演示，运行本机构建目录 `build-terminal-deploy/TerminalPoc.exe`。构建、自测及人工验收步骤见 [Qt 终端技术验证](docs/M0-02_Qt终端技术验证.md)。这是本地模拟终端，不执行系统命令，也不连接远程 Agent；用户已确认 M0-02 验收通过，具体记录见验证说明。

## 当前工程骨架

M1-01 已建立 Client/Agent、Core/Protocol 和 Qt Test 目标，本机 Debug/Release 构建及部署烟测通过。构建、运行、调试入口及已知限制见 [工程骨架与验证](docs/M1-01_工程骨架与验证.md)。M0-03 已有家庭测试电脑（用户确认 Claude CLI 可用、Tailscale 已连接），双端联调待实施；NAS 与正式网络场景延期。真实 Claude 尚未接入；本机 FakeCli 会话服务已由 M3-02 的 --config 入口提供。

## 文件使用顺序

M1-02 已定义核心模型、Provider/终端/会话管理接口，测试入口及生产实现边界见 [核心模型与接口](docs/M1-02_核心模型与接口.md)。应用界面已进入 M4-02 远程 FakeCli 终端；真实 Claude 和附件功能尚未接入；FakeCli 服务见下方 M3-02。

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

Windows ConPTY 后端与真实 FakeCli 集成测试见 [M2-01 说明](docs/M2-01_Windows终端后端.md)。Agent 已在 M3-02 组合真实 FakeCli 会话；Client 基础界面见 M4-01。

## M2-02 本地会话管理

SessionManager 提供创建、查询、附加/分离、控制权租约和显式终止，测试入口见 [M2-02 说明](docs/M2-02_SessionManager.md)。已接入真实后端的集成测试；网络入口已由 M3-02 提供；产品基础界面见 M4-01。

## M3-01 协议编解码

RADP/1 提供控制消息、终端二进制帧、版本协商及错误协议，字段与测试入口见 [M3-01 说明](docs/M3-01_协议编解码.md)。M3-02 已实现本机服务与鉴权路由；家庭 Agent 的跨端连接仍待实测。

## M3-02 Agent WebSocket 服务

使用 --config 加载本地设备摘要、工作区 ACL 与固定 FakeCli 配置，启动经鉴权的常驻 Agent。配置、协议步骤及本机验收见 [M3-02 说明](docs/M3-02_AgentWebSocket服务.md)。默认无配置仍不监听；不自动配置 Tailscale/防火墙，不包含真实 Claude；远程 UI 由下方 M4-02 提供。

## M4-01 Qt Client 基础界面

RemoteClient 默认打开深灰/青绿色原生窗口，支持 FakeAgent 主机状态、项目会话树、多标签、新建、分离和终止确认。界面明确标注模拟，不连接真实 Agent；启动与验证见 [M4-01 说明](docs/M4-01_QtClient基础界面.md)。指定连接参数后的真实终端见下方 M4-02。

## M4-02 远程终端

RemoteClient 使用 `--agent-url ws://127.0.0.1:17890/ --workspace demo` 启动远程模式，弹窗输入 Agent 设备令牌后点击连接。工作区 ID 必须与 Agent 配置和权限一致。支持多个真实 FakeCli 终端、中文/ANSI、输入热键、复制粘贴与尺寸同步；关闭客户端保留远端进程。默认无参数仍是模拟模式。配置、源码和人工验收步骤见 [M4-02 说明](docs/M4-02_远程终端集成.md)。

## M5-01 断线恢复

保持 Agent 运行，Client 断开后点击“连接”，再从工作区打开原会话即可继续输入。重新附加会重建视图并重放有限历史，超限内容和精确画面恢复的限制会持续显示；离线输入不会自动发送。步骤与验证见 [M5-01 说明](docs/M5-01_断线恢复.md)。Agent 重启后的 SQLite 状态迁移由 M5-02 实施。

M5-02 已接入会话元数据与日志；停止 Agent 后重新启动，原活跃会话会显示失效，不能继续附加。双击脚本仍可直接使用，数据位于 .local-run/agent-data。详见 [持久化与诊断验收](docs/M5-02_会话持久化与运行诊断.md)。
