# M3-02 Agent WebSocket 服务

## 范围

基于 M3-01 提交 `f648917`，实现经设备凭据验证的本地 Agent 服务、会话控制和二进制输入输出。默认不监听，显式提供配置才启动。当前 Provider 只有 FakeCli；产品 Client 尚未提供远程 UI，不代表 Claude 或公司到家庭的网络已经验收。

## 源码导航

| 文件（相对仓库根目录） | 职责 |
|---|---|
| [AgentWebSocketServer.h](../core/transport/AgentWebSocketServer.h) | 服务配置、生命周期与审计事件 |
| [AgentWebSocketServer.cpp](../core/transport/AgentWebSocketServer.cpp) | 设备鉴权、RADP 路由、连接清理和背压 |
| [AgentConfiguration.cpp](../apps/RemoteAgent/AgentConfiguration.cpp) | 本地配置文件校验 |
| [main.cpp](../apps/RemoteAgent/main.cpp) | 组装服务、SessionManager 与后端，控制台退出 |
| [FakeCliProvider.cpp](../providers/FakeCli/FakeCliProvider.cpp) | 固定可执行文件与空参数模板，不接受远程命令 |
| [AgentConfigurationTests.cpp](../apps/RemoteAgent/AgentConfigurationTests.cpp) | 配置与 Provider 参数校验 |
| [AgentWebSocketServerTests.cpp](../tests/integration/AgentWebSocketServerTests.cpp) | 真实 WebSocket 与 ConPTY 集成验收 |
| [ADR-017](ADR/017_AgentWebSocket服务.md) | 身份、会话与网络边界 |

## 启动配置

JSON 最多 64 KiB。以下仅为模板，必须替换为本机存在的路径和实际设备令牌摘要；不要在源码仓库中保存真实凭据。客户端 ID 和工作区 ID 使用 1..64 个字母、数字、下划线、点或短横线，首字符为字母或数字。

```json
{
  "bind": "127.0.0.1",
  "port": 17890,
  "clients": [
    { "id": "local-dev", "token_sha256": "REPLACE_WITH_64_LOWERCASE_HEX_SHA256" }
  ],
  "workspaces": [
    { "id": "demo", "path": "D:/Projects/Demo", "allowed_clients": ["local-dev"] }
  ],
  "fake_cli_path": "D:/RADManager/FakeCli.exe"
}
```

`bind` 默认 127.0.0.1，`port` 默认 17890；客户端和工作区分别要求 1..32、1..64 项。未知字段、重复身份/工作区、无效摘要、非法路径及引用未知身份的 ACL 被拒绝；空 ACL 不允许。工作区路径规范化后交由后端再次执行白名单检查。

当前 Agent 使用 4 个并发会话、最多保留 32 条会话（含已退出会话），保证最长合法名称和工作区 ID 的完整列表也能放入单个 64 KiB 消息。达到保留上限后拒绝新建，不自动清除历史；清理接口尚未实现，需在会话收尾后重启 Agent。底层 SessionManager 默认上限未改，组合其他管理器导致列表超帧时服务返回 busy，连接保持可用。

```powershell
cmake --build --preset debug
& .\build-local\apps\RemoteAgent\Debug\RemoteAgent.exe --config 'D:/RADManager/agent.local.json'
```

在 VS 中启动时，将调试参数设为 `--config` 加实际配置文件路径。不带参数仍打印未监听提示并退出 0；`--smoke-test` 继续只验证 SQLite。Ctrl+C 停止 Agent，届时其拥有的会话会关闭；客户端断开不会关闭 Agent 会话。

首次联调使用回环地址。远程启用时，显式替换为家庭电脑实际的本机 Tailscale IPv4 地址；通配地址、普通 LAN 与公网地址被拒绝。程序不创建防火墙规则、不修改 Tailscale 配置，部署前仍需确认授权设备/端口限制。

## 凭据与协议

使用密码学随机源在受控本地环境生成 32 字节随机数，并转为 64 字符小写十六进制令牌。服务端存储的是 **该 64 字符 ASCII 文本的 SHA-256**，不是原始随机字节的摘要。客户端保存令牌并在 WebSocket Upgrade 请求头提供 `Authorization: Bearer <token>`；不要放在 URL、日志或命令行参数中。

本阶段静态配置设备身份，尚无配对 UI、凭据热撤销和 DPAPI 存储。撤销通过删除配置身份并重启生效，重启前应先收尾会话。使用 `ws://` 的加密保护依赖实际 Tailscale 隧道；回环测试也不代表已经验证远程安全部署。

连接后先发送现有 `auth.hello`，收到 `auth.welcome` 后方可发业务请求。身份由握手凭据决定，JSON 不能指定身份；协商本身不是认证。请求结构沿用 [M3-01](M3-01_协议编解码.md)，无需扩展消息类型。

每个身份只允许一个活连接；重复连接不替换原连接。所有 create/list/attach/detach/terminate/resize 通过 SessionManager 执行，工作区 ACL 和单写者规则继续生效。成功 create 仅说明启动受理，等待 process_state=running 后申请 read_write。terminate 响应同样只是受理，最终状态异步送达。

断线自动 detach，重连需重新提供凭据、hello、list、attach。新附加从现有历史重新输出，终端应清空后重建画面；M3-02 不实现精确 ACK 重放。输入 sequence 每次新附加从 0 开始，每成功接收一帧加 1；重复序号拒绝。输出 sequence 为会话字节流的绝对偏移；历史截断发送 terminal.overrun。

WebSocket 地址路径固定为 `/`，不能带 query/fragment 或 Origin。`last_ack_sequence` 虽已在 M3-01 定义字段格式，本阶段路由明确拒绝，避免把有限历史回放伪装成精确恢复。重复 attach 不重置输入序号，只有真正 detach 后的新订阅才重置。

## 资源与故障语义

默认最多 8 个连接，协商期限 5 秒、失联期限 30 秒，每连接发送队列上限 1 MiB，每秒消息上限 256，每连接最多记录 4096 个请求 ID。超过请求记录额度需重新连接；输入重放、非法帧、未授权请求均不执行业务操作。慢客户端超出发送预算时被断开，服务器分离该客户端并保留会话。

每个 RADP 消息最多 64 KiB。服务器限制 Qt 收包和待写缓冲，按预算分批拉取输出，避免单一会话长期占用事件循环。连接 ping/pong 与会话写权租约分别处理；心跳只能续当前写者的租约，不能抢回已失去的控制权。

审计只输出固定事件和经校验的客户端 ID，不输出令牌、请求体或终端内容。当前未持久化审计文件，诊断日志及轮转留到 M5-02。

## 验证入口

```powershell
cmake --preset local-msvc
cmake --build --preset debug
ctest --preset debug -R 'agent\.(configuration|websocket|smoke)'
cmake --build --preset release
ctest --preset release
```

网络测试使用本机回环与固定测试凭据，不接触用户 Claude 登录信息。跨设备 Tailscale、家庭正式出口与 NAS、真实 Claude、完整 UI 和长期断网恢复均需后续单独验收。

Qt 的 pending 队列上限与 HTTP Upgrade 超时不等于完整握手洪泛防护；本阶段验证的是已建立连接、RADP 消息及应用发送预算。控制帧和内核网络缓冲不属于 RADP 应用队列硬上限。部署前继续按 M7 做网络限流与压力验证。关闭控制台窗口或系统关机的优雅退出尚未验收。

## 本次实测记录（2026-10-17）

- Debug / Release 的 RemoteAgent、配置测试、网络测试均编译通过，启用 MSVC `/W4 /WX /analyze`。
- 网络集成 **10 个测试方法**通过（Qt Test 加 init/cleanup 共 12 项）：鉴权、协商与超时、中文与 resize、断线重附加、ACL/多会话隔离、重放与超大帧、发送预算、连接限制、ping 超时、超大列表返回 busy 且保持连接。
- 配置与固定 Provider **2 个测试方法**通过；包含 32 条最坏转义名称仍满足单帧限制的回归。
- Release 全目标构建及全套 **23/23 CTest** 通过，耗时 52.11 秒。随后发现并修复列表大小边界，再次构建受影响目标，Debug/Release 的 `agent.configuration/agent.smoke/agent.websocket` 各 **3/3** 通过，耗时分别 20.52 / 20.58 秒。未重复执行不受此修复影响的全套测试。
- 额外启动真实 Debug RemoteAgent EXE，使用忽略目录内的回环测试配置完成 hello/list/create/attach/echo/terminate；确认日志不含测试令牌，显式终止会话后清理测试 Agent。此流程在最后修复后复验通过。
- clang-format、Git 差异和文档相对链接检查通过。独立审查发现的列表大小问题已加入回归并修复。
- 未执行 Debug WebEngine 全套复验、VS F5 断点、Ctrl+C 退出的自动化测试或跨机器远程联调。先前 Debug WebEngine 超时不因此标为解决。
