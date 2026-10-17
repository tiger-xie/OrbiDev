# M2-02 SessionManager：流程与代码对照

## 范围与入口

2026-10-17，基于 M2-01 提交 9df7806，在 codex/m2-session-manager 工作区实现。SessionManager 实现既有 ISessionManager；测试将它与真实 Windows 后端组合。Client/Agent 应用入口仍为工程骨架，不把本轮测试通过等同于远程 Claude 或 UI 已可用。

| 文件（相对仓库根目录） | 职责 |
|---|---|
| [SessionManager.h](../core/session/SessionManager.h) | 配置、后端工厂及公开方法 |
| [SessionManager.cpp](../core/session/SessionManager.cpp) | 会话生命周期、授权、租约、独立订阅与输出历史 |
| [ISessionManager.h](../core/session/ISessionManager.h) | 既有事件与操作契约，本轮不修改 |
| [WinConPtyBackend.cpp](../platforms/windows/ConPty/WinConPtyBackend.cpp) | 实际进程与管道；管理器只依赖它的抽象接口 |
| [SessionManagerTests.cpp](../tests/unit/SessionManagerTests.cpp) | 可控后端/Provider 替身与边界测试 |
| [SessionManagerIntegrationTests.cpp](../tests/integration/SessionManagerIntegrationTests.cpp) | 真实 FakeCli 多进程验收 |

## 配置和会话操作

调用方在控制线程构造管理器，注入 Provider 实例、可信工作区映射及客户端访问列表、返回独占后端的工厂。RADCore 仅链接 QtCore，不包含 Windows 头或具体 Provider；实际路径验证由 Windows 后端执行。

create 先核验请求、授权、Provider、工作区及额度，再生成 LaunchSpec 并启动后端。同步拒绝不留下会话或完成事件；受理返回 SessionId，不隐式附加。启动/退出结果异步通知，get/list 提供当前快照。

| 操作 | 规则 |
|---|---|
| attach ReadOnly | 独立订阅保留历史，可在进程结束后读取尾部 |
| attach ReadWrite | 仅 Running 可申请；至多一名写者，不抢占 |
| 重复 attach | 同身份同模式幂等；写者可续租；变更模式须先 detach |
| detach | 移除该身份订阅并释放其写权；不停止进程，重复幂等 |
| writeInput / resize | 要求有效写者租约；传播后端队列或尺寸错误 |
| terminate | 创建中仅创建者可取消；运行中要求写者；已退出/正在退出的授权重复请求幂等 |
| list / get / takeOutput | 校验访问权限；拉取输出还须已附加，且长度为正 |

clientId 由未来认证边界传入，不是凭据。静态 ACL 默认拒绝未授权身份；配置不是网络身份认证。当前不支持 worktreeName，不能由客户端任意提交工作目录。

## 租约、容量与输出

默认最多 4 个活跃会话、128 条保留记录、每会话 16 名订阅者、1 MiB 管理器历史。活跃额度在 finished 后释放；满额返回错误。终态记录不悄悄淘汰，清理/持久化留给后续任务。后台仍有 M2-01 的独立有界缓冲。

写者租约默认 30000 毫秒，按单调时钟计时。成功输入、resize 或同模式重复 ReadWrite attach 续租；定时器和控制操作均检查到期。过期释放写权、保留只读订阅，不终止进程；重新申请前先 detach。网络断线到 detach 的映射留给 M3，不声称已验证真实断网。

每订阅者独立游标，不因其他人读取而消费自己的数据。历史超限或后端报告缺口时返回 droppedBytes；不能精确重建已丢失的 VT 画面。后端缺口会舍弃之前不连续的保留前缀，明确计入丢弃量。新附加也报告当前历史之前的丢失。

输出通知合并，订阅代次用于使 detach 前排队的通知失效。完成前归并尾部输出，完成后只读保留历史。管理器销毁是全局资源兜底，不能作为某客户端分离的实现。

## 验证与维护

在已配置的 VS 2022 环境执行：

```powershell
cmake --build --preset debug --parallel
ctest --preset debug -R '^(core.session_manager|session.integration)$'
cmake --build --preset release --parallel
ctest --preset release -R '^(core.session_manager|session.integration)$'
```

JUnit 位于 build-local/tests/unit/{Debug,Release}/session-manager.xml 和 build-local/tests/integration/{Debug,Release}/session-manager-integration.xml。调试时先定位管理器对应方法与状态，再看后端，不将 Provider 逻辑写入终端后端或界面。

本轮 Debug/Release 全目标构建及 MSVC /analyze、/W4 /WX 通过。新增 9 个单元方法及 1 个真实三进程集成方法，两种配置均通过；Debug 两个新 CTest 目标 2/2 通过（3.78 秒），既有核心契约与后端回归 8/8 通过（17.20 秒）。Release 全套 CTest 20/20 通过（31.26 秒），包括 WebEngine、Client/Agent 烟测。clang-format、git diff --check 通过；11 个 core 源/头文件依赖扫描未发现平台、UI 或具体 Provider include。方案取舍见 [ADR-015](ADR/015_SessionManager与控制权租约.md)。不承诺 Agent/Windows 重启后保留原 ConPTY，不包含 M3 协议或 M5 持久化。

### 已知边界

- 上轮 M2-01 的 Debug WebEngine POC 曾重复超时；本轮未重新运行该 Debug 场景，不宣称问题已修复。Release 本轮全套通过不能替代 Debug 环境的专项诊断。
- 管理器在控制线程构造并使用，不支持运行中跨线程迁移。销毁管理器会回收所有进程；分离某客户端只调用 detach。
- 并发额度在后端完成后释放，保留记录达到 128 条则拒绝新建；尚无删除记录或落盘恢复入口。
- 静态 ACL、内存租约与本地测试不代表已完成远程身份认证、真实断网重连或 Claude 验收。
