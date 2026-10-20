# AREC 双端项目实施规划

> 项目代号：AREC（Agent Remote Execution Channel）
>
> 文档状态：M0 基线
>
> 当前实施端：Windows
>
> 最终形态：Mac 控制面 + Mac/Windows 双执行端 + Python Agent

> 实施状态（2026-10-21）：Windows M1-M3 已完成，Debug 单元测试和回环 mock gateway 集成测试通过。

## 1. 项目目标

AREC 要验证一个核心命题：远程工具调用的可靠性不能依赖 Agent 猜测，必须由协议显式表达。

首版需要证明以下能力：

1. Mac 和 Windows 通过同一套协议接入。
2. Agent 可以按工具能力选择执行端，并完成跨机调用。
3. 成功、超时未知、幂等重试、越权四条路径均可演示。
4. 每次调用都有可追溯、已脱敏的结构化审计记录。

首版不是生产级远程执行平台。它的目标是做出一个范围可控、协议清楚、失败路径可解释的工程演示。

## 2. 非目标

以下内容明确不进入首版：

- 不实现屏幕共享、远程桌面、鼠标键盘注入。
- 不做通用 Agent 框架，不引入 LangChain。
- 不引入 Redis、Kafka、etcd、数据库或微服务。
- 不做生产级多租户、TLS 证书管理、密钥轮换或高可用。
- 不追求协议吞吐量，工具执行时间才是主要瓶颈。
- 不实现文件路径沙箱、命令白名单或 OS 级沙箱。
- 首版 Agent 只接 OpenAI 兼容 function calling，不同时实现 Anthropic 适配。

## 3. 系统拓扑

```text
┌──────────────────────────── Mac ────────────────────────────┐
│                                                             │
│  Python Agent                                               │
│  OpenAI 兼容 function calling / dry-run                     │
│           │ HTTP JSON                                       │
│           ▼                                                 │
│  Go Control Plane Gateway                                   │
│  会话令牌 · 工具注册 · 路由 · 审计 · 限流 · 请求状态        │
│           │ 自定义 TCP 帧：4 字节大端长度 + JSON            │
│           ├───────────────────────────────┐                 │
│           ▼                               ▼                 │
│  C++ Data Plane A                  C++ Data Plane B         │
│  macOS / standalone Asio           Windows / standalone Asio│
│           │                               │                 │
│           ▼                               ▼                 │
│      工具子进程                       工具子进程             │
└─────────────────────────────────────────────────────────────┘
```

关键决策：

- 两个 C++ 执行端主动连接 Go 网关，避免 Windows 入站防火墙配置问题。
- Agent 只访问 Go 网关，不直接连接执行端。
- 网关按 `target` 和工具能力路由调用。
- 控制面使用 HTTP JSON，数据面使用长度前缀 JSON 帧。
- 网关和执行端都校验能力令牌，避免只信任网关转发。

## 4. 信任边界

系统包含四个信任边界：

1. **Agent 到网关**：Agent 可能持有过期或被限制的令牌，也可能发起越权工具调用。
2. **网关到执行端**：网关连接可能中断，请求可能已送达但响应丢失。
3. **执行端到工具**：参数可能不合法，命令可能在超时后继续运行。
4. **审计读者到审计记录**：审计必须足以解释行为，但不能泄露密钥和完整敏感参数。

首版采用基础白名单安全边界：

- 能力令牌绑定允许的工具、有效期和最大调用次数。
- 执行端再次验签并检查工具是否在令牌范围内。
- 所有工具参数执行类型、长度和必填项校验。
- `exec_command` 在独立子进程中执行，超时后终止子进程树。
- 审计只记录参数哈希，不记录令牌、Bootstrap Key 或完整敏感参数。

必须明确记录的限制：

- `exec_command` 仍以当前用户权限运行任意 shell 命令。
- `read_file` 可读取当前用户有权访问的任意文件。
- 首版没有 OS 级沙箱，也没有路径根目录或命令前缀限制。
- 局域网 HTTP/TCP 不加密，仅适合受控演示环境。

## 5. 可靠性不变量

实现和测试必须遵守以下不变量：

1. `timeout` 表示结果未知，不表示工具失败。
2. 有副作用的工具超时后不得自动重试。
3. 客户端先创建幂等键，再发起执行。
4. 同一幂等键必须对应同一工具和同一参数；不同请求复用该键时返回 `IDEMPOTENCY_CONFLICT`。
5. 网关失去执行端连接时返回 `unknown`，Agent 必须先查询最终状态。
6. 能力令牌不能扩大工具范围、有效期或调用次数。
7. 审计失败不能阻止安全拒绝，但不能静默丢弃审计错误。
8. 日志和错误响应中不得出现明文令牌、Bootstrap Key、私钥或完整敏感参数。

## 6. 请求状态与错误语义

### 6.1 状态

| 状态 | 含义 | Agent 行为 |
| --- | --- | --- |
| `pending` | 网关已接受请求，尚未派发 | 等待或查询 |
| `running` | 执行端已接收并开始执行 | 等待或查询 |
| `ok` | 工具执行成功 | 使用结果继续 |
| `timeout` | 到达截止时间，执行副作用是否发生未知 | 先查询；仅幂等工具可安全重试 |
| `failed` | 已确认失败，没有未决副作用 | 按错误码处理 |
| `unknown` | 网关无法确认执行端状态 | 先查询，不得直接重试副作用工具 |

状态转换以数据面实际观测为准。网关不能把连接中断推断为 `failed`。

### 6.2 错误码

| 错误码 | 来源 | 默认 retryable |
| --- | --- | --- |
| `TOOL_NOT_FOUND` | 网关 | `false` |
| `UNAUTHORIZED` | 网关或执行端 | `false` |
| `RATE_LIMITED` | 网关 | `true`，退避后 |
| `INVALID_PARAMS` | 执行端 | `false` |
| `EXECUTION_TIMEOUT` | 执行端 | 由工具幂等性决定 |
| `EXECUTION_FAILED` | 执行端 | 由具体失败类型决定 |
| `OUTPUT_TOO_LARGE` | 执行端 | `false` |
| `PROTOCOL_ERROR` | 网关或执行端 | `false` |
| `IDEMPOTENCY_CONFLICT` | 执行端 | `false` |
| `REQUEST_STATE_UNAVAILABLE` | 执行端 | `false`，结果状态为 `unknown` |

`retryable` 是服务端建议，不是 Agent 无需判断的授权。Agent 最终重试策略还必须考虑工具是否幂等和业务上下文。

## 7. HTTP 控制面接口

除 `POST /session` 外，接口统一使用：

```http
Authorization: Bearer <capability-token>
Content-Type: application/json
```

### 7.1 创建会话

```http
POST /session
X-AREC-Bootstrap: <bootstrap-key>
```

请求体：

```json
{
  "allowed_tools": ["system_info", "exec_command", "read_file"],
  "ttl_seconds": 900,
  "max_calls": 100
}
```

响应：

```json
{
  "token": "<REDACTED>",
  "token_id": "token-uuid",
  "session_id": "session-uuid",
  "expires_at": "2026-10-20T10:15:00Z"
}
```

网关必须校验请求的工具范围不超过服务端允许范围。默认有效期为 15 分钟，默认最大调用次数为 100。

### 7.2 获取授权工具

```http
GET /tools
```

响应只包含当前令牌允许且当前有执行端可用的工具。每项至少包含：

```json
{
  "name": "read_file",
  "description": "读取文件内容",
  "platform": ["darwin", "windows"],
  "idempotent": true,
  "params_schema": {}
}
```

### 7.3 发起执行

```http
POST /execute
```

请求体：

```json
{
  "tool": "exec_command",
  "params": {
    "command": "git --version",
    "cwd": "C:\\workspace"
  },
  "target": "auto",
  "idempotency_key": "client-generated-unique-key",
  "deadline_ms": 5000
}
```

响应体：

```json
{
  "request_id": "request-uuid",
  "status": "ok",
  "result": {
    "stdout": "git version 2.52.0.windows.1",
    "stderr": "",
    "exit_code": 0
  },
  "error_code": null,
  "retryable": false
}
```

状态为 `timeout` 或 `unknown` 时，必须保留 `request_id`，并建议 Agent 查询。

### 7.4 查询请求状态

```http
GET /query/{request_id}
```

只允许同一会话查询。响应包含最新状态、结果或错误码、是否可重试及最后更新时间。

## 8. 数据面协议

### 8.1 帧格式

```text
[4 字节大端长度][UTF-8 JSON payload]
```

- 单帧最大 4 MiB。
- 长度为 0、长度超限、JSON 非法或缺少必需字段时返回 `PROTOCOL_ERROR`。
- 单连接允许通过 `request_id` 多路复用多个请求和响应。
- JSON 消息增加 `protocol_version: "1"`。

### 8.2 消息类型

```json
{
  "type": "register",
  "protocol_version": "1",
  "agent_id": "windows-b",
  "platform": "windows",
  "tools": []
}
```

```json
{
  "type": "register_ack",
  "protocol_version": "1",
  "accepted": true
}
```

```json
{
  "type": "request",
  "protocol_version": "1",
  "request_id": "request-uuid",
  "session_id": "session-uuid",
  "tool": "read_file",
  "params": {
    "path": "C:\\workspace\\README.md",
    "max_bytes": 65536
  },
  "idempotency_key": "client-generated-unique-key",
  "deadline_ms": 5000,
  "token": "<REDACTED>"
}
```

```json
{
  "type": "response",
  "protocol_version": "1",
  "request_id": "request-uuid",
  "status": "timeout",
  "error_code": "EXECUTION_TIMEOUT",
  "retryable": false,
  "message": "deadline exceeded; execution state unknown",
  "executed_at": "2026-10-20T10:00:00Z",
  "platform": "windows"
}
```

```json
{
  "type": "query",
  "protocol_version": "1",
  "request_id": "request-uuid"
}
```

```json
{
  "type": "query_response",
  "protocol_version": "1",
  "request_id": "request-uuid",
  "status": "ok",
  "result": {}
}
```

执行端连接断开后按 1 秒、2 秒、4 秒递增重连，最大间隔 30 秒并加入少量抖动。重连成功后重新发送注册消息。

## 9. 能力令牌

能力令牌使用两段 Base64URL 编码：

```text
<base64url(payload)>.<base64url(HMAC-SHA256(payload, secret))>
```

Payload 至少包含：

```json
{
  "version": 1,
  "token_id": "token-uuid",
  "session_id": "session-uuid",
  "allowed_tools": ["system_info", "read_file"],
  "issued_at": 1760954400,
  "expires_at": 1760955300,
  "max_calls": 100
}
```

规则：

- `AREC_HMAC_SECRET` 由环境变量注入，网关和两个执行端持有同一密钥。
- `AREC_BOOTSTRAP_KEY` 只用于创建会话，不进入执行请求。
- 网关负责累计调用次数和限流，执行端负责验签、过期检查和工具范围检查。
- 令牌过期、签名错误、工具越权均返回 `UNAUTHORIZED`。
- 任何日志、错误响应和审计记录都不得保存原始令牌。

## 10. 工具契约

### 10.1 首版最小工具

| 工具 | 参数 | 平台 | 幂等 | Windows 当前阶段 |
| --- | --- | --- | --- | --- |
| `system_info` | 无 | Mac/Windows | 是 | 实现 |
| `exec_command` | `command`, `cwd?` | Mac/Windows | 否 | 实现 |
| `read_file` | `path`, `max_bytes?` | Mac/Windows | 是 | 实现 |

### 10.2 完整 MVP 工具

| 工具 | 参数 | 平台 | 幂等 | 阶段 |
| --- | --- | --- | --- | --- |
| `system_info` | 无 | Mac/Windows | 是 | M2 |
| `exec_command` | `command`, `cwd?` | Mac/Windows | 否 | M2 |
| `read_file` | `path`, `max_bytes?` | Mac/Windows | 是 | M2 |
| `write_file` | `path`, `content` | Mac/Windows | 是 | M5 |
| `list_dir` | `path` | Mac/Windows | 是 | M5 |
| `create_dir` | `path` | Mac/Windows | 是 | M5 |

工具契约原则：

- 参数是领域概念，不直接暴露 shell 开关。
- `exec_command` 返回 `stdout`、`stderr`、`exit_code` 三个独立字段。
- 路径类工具返回结构化结果，不返回拼接后的日志文本。
- 输出需要截断时返回 `truncated: true`，并遵守声明的最大字节数。
- 有副作用的工具必须明确声明 `idempotent`。

### 10.3 首版限制

- `exec_command` 的 `stdout` 和 `stderr` 每路最多 256 KiB。
- `read_file` 的默认和最大读取量均为 256 KiB；超出时截断并标记。
- `deadline_ms` 默认 5000，最大 30000。
- 执行端缓存和请求状态默认保留 10 分钟，最多 1024 条。
- `write_file` 在 M5 使用临时文件加原子替换，避免出现半写文件。
- `create_dir` 对已存在目录视为成功。

## 11. Windows 实施说明

### 11.1 当前环境

本机已确认存在：

- Visual Studio Community 2022 17.14。
- MSVC 14.44。
- Windows SDK 10.0.26100.0。
- VS 自带 CMake 3.31.6。
- VS 自带 vcpkg 2025-11-19。

CMake、MSVC 和 vcpkg 尚未加入普通 PowerShell 的 PATH。实施时使用 Developer PowerShell，或在命令中调用绝对路径。

### 11.2 构建选型

- C++17。
- CMake。
- MSVC。
- VS 自带 vcpkg，使用 manifest 模式。
- standalone Asio。
- nlohmann-json。
- CTest。

计划目录：

```text
arec/
├─ CMakeLists.txt
├─ vcpkg.json
├─ data_plane/
│  ├─ include/arec/
│  ├─ src/
│  └─ tests/
├─ protocol/
├─ gateway/
├─ agent/
└─ docs/
```

Windows 第一阶段只实现 `data_plane/`、共享协议代码和本地测试，不要求在本机运行 Go 网关。

### 11.3 Windows 协议骨架

M1 必须完成：

- TCP 反向连接和自动重连。
- 长度前缀帧增量解码，正确处理分片和粘包。
- `register`、`register_ack`、`request`、`response`、`query`、`query_response`。
- 未知消息、非法 JSON 和超大帧的安全拒绝。
- 单连接多路复用 `request_id`。
- 启动时根据工具表生成注册清单。

### 11.4 Windows 工具执行

`system_info`：

- 返回 `hostname`、`os`、`arch`。
- 使用 Windows 原生 API 获取系统信息。

`read_file`：

- 使用 `std::filesystem` 或 Win32 文件 API。
- 支持 `max_bytes`，默认及上限为 256 KiB。
- 返回 `path`、`content`、`bytes_read`、`truncated`。

`exec_command`：

- 使用 `CreateProcessW` 启动 `cmd.exe /c <command>`。
- 使用匿名管道捕获 `stdout` 和 `stderr`。
- 将子进程放入启用 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object。
- 超时或连接关闭时终止整个 Job Object。
- 返回 `stdout`、`stderr`、`exit_code` 和截断标记。

### 11.5 请求执行流程

```text
读取完整帧
  -> 解析 JSON 和协议版本
  -> 校验令牌、工具范围和参数
  -> 计算工具名与参数哈希
  -> 命中同键同哈希缓存：返回缓存
  -> 命中同键不同哈希：返回 IDEMPOTENCY_CONFLICT
  -> 标记 running 并派发工具
  -> 保存最终状态
  -> 返回 response
```

超时路径：

```text
deadline 到达
  -> 尝试终止工具进程或停止工作线程
  -> 标记 timeout
  -> 对 exec_command 返回 retryable=false
  -> 保存状态供 query 使用
```

## 12. 里程碑

### M0 文档基线

交付：

- `docs/AREC_PROJECT_PLAN.md`
- `docs/AREC_MAC_IMPLEMENTATION.md`

退出标准：

- 状态、错误码、接口字段和里程碑范围在双端文档中一致。

### M1 Windows 协议骨架

交付：

- CMake/vcpkg 工程。
- 帧编解码和消息分派。
- 反向连接、注册、自动重连。
- 进程内 mock gateway。

退出标准：

- 分片、粘包、非法 JSON、超大帧和未知消息测试通过。
- mock gateway 能收到注册并发送查询。

### M2 Windows 最小工具

交付：

- `system_info`、`exec_command`、`read_file`。
- 参数校验、截止时间、输出限制。
- 幂等缓存和状态查询。

退出标准：

- 三个工具的成功路径和错误路径测试通过。
- `exec_command` 超时后确认子进程树被终止。

### M3 Windows 本地可靠性验收

交付：

- 进程内 mock gateway 集成测试。
- 故障注入脚本或测试用例。
- 结构化测试报告。

退出标准：

- 成功、超时未知、幂等重放、越权、断线查询、协议错误全部通过。

### M4 Mac 控制面与跨机联调

交付：

- Go 网关。
- Python Agent。
- Mac C++ 数据面。
- 最小 3 工具双端互通。

退出标准：

- Agent 可以通过 `darwin` 和 `windows` 明确目标执行工具。
- `auto` 能选择支持目标工具的执行端。
- Windows 主动连接 Mac 网关成功。

### M5 完整 MVP

交付：

- 双端补齐 `write_file`、`list_dir`、`create_dir`。
- 四类可靠性演示。
- JSONL 审计和脱敏检查。
- 面试演示说明和运行手册。

退出标准：

- 双端 6 个工具可用。
- 成功、超时、幂等、越权四条路径可以连续演示。
- 审计可以回答会话、令牌、工具、参数哈希、目标平台和最终状态。

## 13. 测试计划

### 13.1 单元测试

- 一次读取包含多个帧。
- 一个帧跨多次读取。
- 长度为 0、超过 4 MiB、声明长度大于实际数据。
- 非法 JSON、缺少 `type`、未知 `type`。
- 令牌签名错误、过期、工具越权。
- 参数缺失、类型错误、字符串超长。
- 输出截断和 `OUTPUT_TOO_LARGE`。
- 幂等键命中、参数哈希不同导致冲突、缓存过期。

### 13.2 Windows 集成测试

| 场景 | 预期 |
| --- | --- |
| `system_info` 成功 | `status=ok`，包含 Windows 平台信息 |
| `read_file` 成功 | 返回内容和读取字节数 |
| `read_file` 超限 | `truncated=true`，不超过限制 |
| `exec_command` 成功 | 标准输出、错误输出和退出码分离 |
| `exec_command` 超时 | `status=timeout`、`retryable=false`、进程树终止 |
| 相同幂等键重复调用 | 执行一次，两次返回一致结果 |
| 相同键不同参数 | `IDEMPOTENCY_CONFLICT` |
| 未授权工具 | `UNAUTHORIZED`，工具不执行 |
| 连接在请求后断开 | 网关记录 `unknown`，查询可恢复最终状态 |
| 非法帧 | `PROTOCOL_ERROR`，进程不崩溃 |

### 13.3 跨机验收

- Mac Agent 调 Mac `system_info`。
- Mac Agent 调 Windows `system_info`。
- `auto` 路由到支持目标工具的执行端。
- Windows 执行端重启后自动重连并重新注册。
- 请求执行中短暂断网，重连后通过 `query` 获取结果。
- 审计中没有明文令牌、Bootstrap Key 或完整敏感参数。

## 14. 演示脚本

### 14.1 成功路径

1. Mac Agent 获取令牌和工具列表。
2. 调用 Windows `system_info`。
3. 返回 Windows 主机信息。
4. 展示审计中的会话、平台、状态和耗时。

### 14.2 超时未知

1. 调用 Windows `exec_command` 执行一个超过截止时间的命令。
2. 返回 `status=timeout` 和 `retryable=false`。
3. Agent 不重试，先查询同一 `request_id`。
4. 展示执行端进程树已被终止。

### 14.3 幂等重试

1. 使用只读工具和固定幂等键发起调用。
2. 模拟客户端超时或重复提交。
3. 执行端直接返回缓存结果。
4. 展示工具只执行一次。

### 14.4 越权路径

1. 使用只包含 `system_info` 的令牌调用 `exec_command`。
2. 网关或执行端返回 `UNAUTHORIZED`。
3. 展示工具没有执行，审计记录了拒绝原因。

## 15. 风险与取舍

| 风险 | 影响 | 处理 |
| --- | --- | --- |
| 明文局域网协议 | 令牌和结果可被同网段观察 | 仅用于受控演示，文档显式标注 |
| 无 OS 级沙箱 | 工具拥有当前用户权限 | 不声称生产安全，限制令牌工具范围并审计 |
| Windows 进程树终止复杂 | 超时后命令可能残留 | 使用 Job Object 并做进程清理测试 |
| 幂等缓存只保留 10 分钟 | 超出窗口无法去重 | 在文档和演示中说明 TTL |
| 断线后状态可能无法立即恢复 | Agent 间歇收到 `unknown` | 强制先查询，副作用工具禁止自动重试 |
| 自动路由依赖注册状态 | 执行端离线时调用失败 | 返回明确的 `TOOL_NOT_FOUND` 或不可用状态 |
| 功能范围扩大 | 首版迟迟无法演示 | 严格按 M1 至 M5 推进，M5 前不做生产化增强 |

## 16. 面试讲述重点

建议围绕三个工程判断展开：

1. **为什么需要协议，而不是让 Agent 重试**：分布式调用存在请求未到、执行未完成、响应丢失和副作用已发生四种状态，Agent 无法通过猜测区分。
2. **为什么引入幂等键和查询**：幂等键让服务端承担去重责任，查询让网关和 Agent 能恢复未决状态。
3. **为什么先做 Windows 最小闭环**：先用 mock gateway 证明协议和失败语义，再接入 Mac 控制面和真实局域网，避免同时调试三台机器和多个工具。

回答取舍时应明确：

- 首版是受控环境下的可靠性演示，不是生产级远程执行平台。
- 基础白名单不能替代 OS 级沙箱。
- 协议层的可靠性状态比模型提示词更重要。

## 17. 环境变量约定

所有秘密只通过环境变量注入：

| 变量 | 使用方 | 说明 |
| --- | --- | --- |
| `AREC_BOOTSTRAP_KEY` | Go 网关、Agent 创建会话 | 会话创建凭据 |
| `AREC_HMAC_SECRET` | Go 网关、两个 C++ 执行端 | 能力令牌签名与验证 |
| `OPENAI_API_KEY` | Python Agent | 模型 API 密钥 |
| `OPENAI_BASE_URL` | Python Agent | OpenAI 兼容服务地址 |
| `OPENAI_MODEL` | Python Agent | 支持 function calling 的模型标识 |

示例值只能使用 `YOUR_BOOTSTRAP_KEY_HERE`、`YOUR_HMAC_SECRET_HERE`、`YOUR_API_KEY_HERE` 等占位符，不允许写入真实值。

## 18. 当前步骤

本轮只完成 M0 文档基线，不创建构建系统或业务代码。后续实施从 M1 Windows 协议骨架开始。
