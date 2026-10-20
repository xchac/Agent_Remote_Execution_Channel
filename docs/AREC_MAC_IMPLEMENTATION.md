# AREC Mac 专属实施手册

> 适用范围：Mac 控制面、Mac C++ 数据面、Python Agent
>
> 目标机器：用户在 Mac 上后续实施
>
> 当前 Windows 机器不执行本文命令
>
> 协议与验收基线：`docs/AREC_PROJECT_PLAN.md`

## 1. 本文范围

本文只覆盖 Mac 上的工作：

- Go 控制面网关。
- Python Agent。
- Mac C++ 数据面。
- Mac 局域网启动、测试和 Windows 互操作。

本文不在 Windows 本机运行。Windows 数据面的实现与本地 mock gateway 验收见总规划。

首版 Mac 先完成最小 3 个工具：

- `system_info`
- `exec_command`
- `read_file`

M5 再补齐：

- `write_file`
- `list_dir`
- `create_dir`

## 2. 实施前冻结的契约

以下字段必须与总规划完全一致：

- HTTP 鉴权：`Authorization: Bearer <capability-token>`。
- 会话创建：`X-AREC-Bootstrap: <bootstrap-key>`。
- 数据面帧：4 字节大端长度加 JSON，最大 4 MiB。
- 协议版本：`protocol_version: "1"`。
- 状态：`pending | running | ok | timeout | failed | unknown`。
- 错误码：以总规划第 6.2 节为准，查询状态过期或不存在时使用 `REQUEST_STATE_UNAVAILABLE` 并返回 `unknown`。
- 默认令牌有效期：15 分钟。
- 默认最大调用次数：100。
- 默认请求截止时间：5000 ms，最大 30000 ms。
- 幂等缓存和请求状态保留：10 分钟，最多 1024 条。

任何新增字段或错误码都应先更新总规划，避免 Mac 和 Windows 各自实现一套语义。

## 3. Mac 环境准备

### 3.1 推荐版本

- macOS：用户届时确认实际版本。
- Xcode Command Line Tools：最新稳定版。
- Homebrew：最新稳定版。
- Go：1.24 或更新版本。
- Python：3.12 或更新版本。
- CMake：3.25 或更新版本。
- Ninja：可选，推荐安装。
- Apple Clang：由 Xcode Command Line Tools 提供。

### 3.2 安装依赖

先确认工具状态：

```bash
xcode-select -p
brew --version
go version
python3 --version
cmake --version
```

缺少 Xcode Command Line Tools 时执行：

```bash
xcode-select --install
```

安装 Homebrew 后安装项目依赖：

```bash
brew update
brew install go cmake ninja asio nlohmann-json python@3.12
```

验证 C++ 编译环境：

```bash
clang++ --version
cmake --version
```

### 3.3 Python 虚拟环境

在仓库根目录执行：

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
```

Agent 依赖写入 `agent/requirements.txt`，首版至少包含：

```text
openai
requests
```

安装：

```bash
python -m pip install -r agent/requirements.txt
```

不要把虚拟环境、API Key 或本地配置提交到 Git。

## 4. 推荐目录结构

```text
arec/
├─ gateway/
│  ├─ cmd/arec-gateway/main.go
│  ├─ internal/auth/
│  ├─ internal/audit/
│  ├─ internal/registry/
│  ├─ internal/router/
│  ├─ internal/session/
│  └─ internal/transport/
├─ agent/
│  ├─ arec_agent/
│  │  ├─ client.py
│  │  ├─ dry_run.py
│  │  └─ llm_loop.py
│  ├─ tests/
│  ├─ requirements.txt
│  └─ main.py
├─ data_plane/
│  ├─ include/arec/
│  ├─ src/
│  └─ tests/
├─ protocol/
├─ docs/
├─ CMakeLists.txt
└─ vcpkg.json
```

共享 JSON 字段和常量的来源优先放在 `protocol/`，Go 和 C++ 实现按同一份契约编写。首版不引入代码生成工具。

## 5. Go 控制面实施

### 5.1 运行形态

单进程、内存态，不接数据库：

- HTTP 控制面监听一个端口。
- 数据面 TCP 监听另一个端口。
- 工具注册表保存在内存。
- 请求状态和幂等映射保存在内存。
- 审计追加写入 JSONL 文件。

建议默认端口：

- HTTP 控制面：`8080`。
- 数据面 TCP：`9000`。

### 5.2 初始化模块

在 `gateway/` 下执行：

```bash
go mod init arec/gateway
go get golang.org/x/time/rate
```

如果后续把项目推送到远程 Git 仓库，再统一修改 module path，不在实现中途混用多个路径。

### 5.3 推荐包职责

| 包 | 职责 |
| --- | --- |
| `session` | 创建会话、生成 token payload、累计调用次数 |
| `auth` | HMAC-SHA256 签名、验签、过期和工具范围检查 |
| `registry` | 保存执行端连接、平台、工具清单和在线状态 |
| `router` | 处理 `darwin`、`windows`、`auto` 路由 |
| `transport` | 长度前缀帧编解码、连接生命周期、请求响应关联 |
| `audit` | JSONL 写入、参数哈希和敏感字段脱敏 |
| `cmd/arec-gateway/main.go` | 配置读取、服务启动、优雅退出 |

### 5.4 HTTP 处理器

按总规划实现：

- `POST /session`
- `GET /tools`
- `POST /execute`
- `GET /query/{request_id}`

`POST /session` 处理顺序：

1. 从请求头读取 Bootstrap Key。
2. 使用常量时间比较校验服务端环境变量。
3. 校验请求的工具范围是服务端允许范围的子集。
4. 应用默认或请求中的 TTL 和调用次数上限。
5. 生成 `session_id`、`token_id` 和签名 token。
6. 返回 token，不把 Bootstrap Key 写入响应或日志。

`GET /tools` 处理顺序：

1. 验证 token。
2. 取 token 中的工具白名单。
3. 与在线执行端注册表求交集。
4. 返回可用工具和 schema。

`POST /execute` 处理顺序：

1. 验证 token、调用次数和限流。
2. 验证工具字段、目标平台和幂等键。
3. 生成网关 `request_id`。
4. 根据 `target` 选择执行端。
5. 创建 `pending` 状态并发送数据面请求。
6. 将响应更新为最终状态。
7. 连接中断时更新为 `unknown`，不使用 `failed`。
8. 写审计并返回结构化响应。

### 5.5 路由规则

`target: "darwin"`：

- 只选择在线的 Mac 执行端。
- 无可用执行端时返回 `TOOL_NOT_FOUND`。

`target: "windows"`：

- 只选择在线的 Windows 执行端。
- 无可用执行端时返回 `TOOL_NOT_FOUND`。

`target: "auto"`：

- 过滤出声明支持该工具的执行端。
- 首版优先选择同会话最近使用且健康的执行端。
- 多个候选时按固定优先级选择，建议 `darwin` 优先，保证演示结果可预测。
- 最终结果必须包含实际执行的平台。

不要把“同一工具多个平台”理解为可以无提示自动重试。路由只发生在首次派发。

### 5.6 限流

使用 `golang.org/x/time/rate`：

- 每个会话一个令牌桶。
- 每类工具一个令牌桶。
- 超限返回 `RATE_LIMITED`、`retryable=true`。
- 不重试已经派发给执行端的请求。

### 5.7 审计

建议文件名：

```text
var/audit/arec-audit.jsonl
```

每条记录至少包含：

```json
{
  "timestamp": "2026-10-20T10:00:00Z",
  "session_id": "session-uuid",
  "token_id": "token-uuid",
  "request_id": "request-uuid",
  "tool": "exec_command",
  "params_hash": "sha256-hex",
  "target_platform": "windows",
  "status": "timeout",
  "error_code": "EXECUTION_TIMEOUT",
  "duration_ms": 5002,
  "executed_at": "2026-10-20T10:00:05Z"
}
```

禁止写入：

- 原始 capability token。
- Bootstrap Key。
- HMAC secret。
- OpenAI API Key。
- 完整文件内容。
- 未脱敏的命令输出。

## 6. Python Agent 实施

### 6.1 配置

通过环境变量读取：

| 变量 | 说明 |
| --- | --- |
| `AREC_GATEWAY_URL` | 默认 `http://127.0.0.1:8080` |
| `AREC_BOOTSTRAP_KEY` | 创建会话凭据 |
| `OPENAI_API_KEY` | 模型 API 密钥 |
| `OPENAI_BASE_URL` | OpenAI 兼容服务地址 |
| `OPENAI_MODEL` | 支持 function calling 的模型标识 |

安全输入示例：

```bash
read -r -s AREC_BOOTSTRAP_KEY
export AREC_BOOTSTRAP_KEY

read -r -s OPENAI_API_KEY
export OPENAI_API_KEY
```

不要把真实值写进 shell 脚本、`.env` 示例、README 或 Git 提交。

### 6.2 dry-run 模式

`--dry-run` 不调用模型，只执行固定工具序列：

1. 创建会话。
2. 获取授权工具。
3. 调用 `system_info`。
4. 如果允许，调用 `exec_command` 执行只读命令。
5. 调用 `read_file` 读取一个显式测试文件。
6. 输出每一步的 `request_id` 和状态。

dry-run 是跨机联调的第一道门槛。它通过后才接真实模型。

### 6.3 模型工具循环

主循环：

```text
加载工具 schema
  -> 转换 OpenAI tools 格式
  -> 调用模型
  -> 解析 tool_calls
  -> 为每个调用生成 idempotency_key
  -> POST /execute
  -> ok：回灌 tool result
  -> timeout 或 unknown：GET /query/{request_id}
  -> retryable=false：停止该调用
  -> UNAUTHORIZED：终止并报告
  -> 继续下一轮模型调用
```

建议为每个 `tool_call.id` 持久保存幂等键，至少覆盖当前 Agent 进程生命周期。不要在重试时重新生成幂等键。

### 6.4 重试规则

允许自动重试：

- 网关明确返回 `RATE_LIMITED`，使用指数退避。
- 只读工具因连接问题返回 `unknown`，查询确认未执行后可以重试。

禁止自动重试：

- `exec_command` 的 `timeout` 或 `unknown`。
- `UNAUTHORIZED`。
- `INVALID_PARAMS`。
- `IDEMPOTENCY_CONFLICT`。
- `PROTOCOL_ERROR`。

任何重试都必须复用原幂等键或使用服务端确认后的新键，不能仅凭 HTTP 层异常重新提交副作用工具。

### 6.5 输出

Agent 输出应包含：

- 模型生成的工具调用意图。
- 网关 `request_id`。
- 工具状态和结果摘要。
- 超时或未知时的查询结果。
- 最终是否重试及原因。

不应打印完整令牌或敏感参数。

## 7. Mac C++ 数据面实施

### 7.1 构建配置

使用 C++17、CMake 和 Apple Clang。

Homebrew 依赖：

```bash
brew install asio nlohmann-json cmake ninja
```

CMake 查找：

```cmake
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

如果本机 Homebrew CMake 配置路径不在默认搜索路径，使用：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

### 7.2 与 Windows 共享的行为

Mac 和 Windows 必须保持一致：

- 帧格式和 4 MiB 限制。
- 协议版本和消息类型。
- 令牌 payload、签名算法和字段名。
- 状态、错误码和 `retryable` 语义。
- 幂等键和参数哈希算法。
- 10 分钟、1024 条缓存限制。
- 注册、自动重连和查询协议。

平台差异只放在工具执行层。

### 7.3 启动与连接

Mac 数据面启动后：

1. 读取网关地址、`agent_id`、`AREC_HMAC_SECRET`。
2. 主动连接网关数据面端口。
3. 发送 `register`，声明 `platform: "darwin"` 和工具清单。
4. 等待 `register_ack`。
5. 保持连接并处理请求、查询。
6. 断线后按指数退避重连并重复注册。

推荐测试时让 Mac 数据面连接 `127.0.0.1:9000`。

### 7.4 工具执行

`system_info`：

- 使用 macOS 的系统 API 获取主机名、系统版本和架构。
- 返回结构化字段，不拼接自由文本。

`read_file`：

- 使用 `std::filesystem` 或 POSIX 文件 API。
- 默认和最大读取量为 256 KiB。
- 超出限制时返回 `truncated=true`。

`exec_command`：

- 使用 `/bin/sh -c <command>`。
- 捕获 `stdout` 和 `stderr`。
- 每路最多 256 KiB。
- 超时后终止整个进程组，不只终止 shell 父进程。
- 返回 `stdout`、`stderr`、`exit_code`。

### 7.5 请求处理

每个请求：

1. 验证协议版本和必需字段。
2. 验证 token 签名、过期时间和工具范围。
3. 验证参数 schema。
4. 计算工具名和规范化参数哈希。
5. 检查幂等缓存。
6. 创建 `running` 状态并执行工具。
7. 保存最终状态并发送 `response`。

`query` 只能返回该执行端实际见过的 `request_id`。缓存不存在时返回明确的未找到状态，不能伪造成功或失败。

## 8. 本机启动顺序

以下命令在 Mac 上执行。

### 8.1 终端一：Go 网关

先安全输入环境变量：

```bash
read -r -s AREC_BOOTSTRAP_KEY
export AREC_BOOTSTRAP_KEY

read -r -s AREC_HMAC_SECRET
export AREC_HMAC_SECRET

export AREC_HTTP_ADDR=":8080"
export AREC_DATA_ADDR=":9000"
export AREC_AUDIT_PATH="var/audit/arec-audit.jsonl"

cd gateway
go run ./cmd/arec-gateway
```

### 8.2 终端二：Mac 数据面

```bash
read -r -s AREC_HMAC_SECRET
export AREC_HMAC_SECRET

export AREC_GATEWAY_DATA_ADDR="127.0.0.1:9000"
export AREC_AGENT_ID="mac-a"

./build/arec-data-plane
```

### 8.3 终端三：Python Agent

```bash
source .venv/bin/activate

read -r -s AREC_BOOTSTRAP_KEY
export AREC_BOOTSTRAP_KEY

export AREC_GATEWAY_URL="http://127.0.0.1:8080"

python -m agent.main --dry-run
```

真实模型模式：

```bash
read -r -s OPENAI_API_KEY
export OPENAI_API_KEY

export OPENAI_BASE_URL="YOUR_OPENAI_COMPATIBLE_BASE_URL"
export OPENAI_MODEL="YOUR_MODEL_ID"

python -m agent.main
```

`OPENAI_BASE_URL` 和 `OPENAI_MODEL` 使用配置值，不写真实密钥。

## 9. 局域网与 Windows 联调

### 9.1 网络准备

- Mac 和 Windows 连接同一局域网。
- 记录 Mac 的局域网地址。
- 让 Windows 主动连接 Mac 的数据面端口，避免 Windows 入站监听。
- 仅允许受控测试网段访问网关端口。

只读查看 Mac 地址：

```bash
ipconfig getifaddr en0
```

如果实际网络接口不是 `en0`，使用：

```bash
ifconfig
```

### 9.2 Windows 执行端配置

在 Windows 上设置：

```powershell
$env:AREC_GATEWAY_DATA_ADDR = "<MAC_LAN_IP>:9000"
$env:AREC_AGENT_ID = "windows-b"
$env:AREC_HMAC_SECRET = '<REDACTED>'
```

使用环境变量或受控进程环境注入，不把真实 secret 写入脚本和仓库。

### 9.3 联调顺序

1. Mac 网关启动并通过本机健康检查。
2. Mac 数据面连接并完成 `register_ack`。
3. Windows 数据面连接并完成 `register_ack`。
4. `GET /tools` 同时看到两个平台。
5. dry-run 调用 Mac `system_info`。
6. dry-run 调用 Windows `system_info`。
7. dry-run 使用 `auto` 调用一个双端支持的工具。
8. 接入真实模型，执行一次跨机任务。

### 9.4 防火墙检查

如果 Windows 连接失败，依次检查：

- Mac 数据面监听地址是否为局域网可达地址，而不是仅回环地址。
- macOS 防火墙是否允许对应进程。
- Windows 是否能解析和访问 Mac 局域网 IP。
- 两个进程是否使用相同数据面端口。
- 服务端日志是否出现 TCP 连接和 `register` 消息，而不是持续 SYN 超时。

不要在排障日志中打印 token 或 HMAC secret。

## 10. 测试计划

### 10.1 Go 网关单元测试

- Bootstrap Key 正确、错误和缺失。
- token 签名、过期、工具越权和最大调用次数。
- `target` 路由和能力过滤。
- `auto` 选择顺序。
- 请求状态从 `pending` 到最终状态。
- 连接中断产生 `unknown`。
- 审计参数哈希和脱敏。

### 10.2 Python Agent 测试

- dry-run 固定序列。
- `ok` 结果回灌模型。
- `timeout` 先查询，不立即重试。
- `unknown` 先查询。
- `UNAUTHORIZED` 终止。
- `RATE_LIMITED` 退避。
- 相同 `tool_call.id` 复用幂等键。
- 日志不包含真实 key。

### 10.3 Mac 数据面测试

- 分片和粘包。
- 非法 JSON、未知消息、超大帧。
- token 验签和工具范围。
- `system_info` 成功。
- `read_file` 成功和截断。
- `exec_command` 成功、非零退出码和超时。
- 超时后进程组终止。
- 幂等命中和冲突。
- `query` 返回最终状态。

### 10.4 跨机验收

| 场景 | 预期 |
| --- | --- |
| Mac 调 Mac 工具 | `ok`，平台为 `darwin` |
| Mac 调 Windows 工具 | `ok`，平台为 `windows` |
| `auto` 路由 | 返回实际执行平台 |
| Windows 重启后重连 | 重新注册并可调用 |
| 执行中断网 | 网关状态先为 `unknown`，恢复后查询得到最终状态 |
| 越权调用 | `UNAUTHORIZED`，执行端无副作用 |
| 审计检查 | 无原始 token、Bootstrap Key、API Key |

## 11. 演示流程

建议固定演示顺序：

1. 展示 `GET /tools` 中双端能力。
2. 调 Mac `system_info`。
3. 调 Windows `system_info`。
4. 用 `auto` 调用 `read_file`。
5. 触发 `exec_command` 超时，展示 `timeout` 和 `retryable=false`。
6. 查询同一 `request_id`，展示最终状态。
7. 用只读工具重复幂等键，展示只执行一次。
8. 用受限 token 调用未授权工具，展示拒绝。
9. 打开审计 JSONL，展示可追溯且已脱敏。

不要把演示顺序设计成先执行有副作用的超时调用再自动重试，这会直接破坏可靠性叙事。

## 12. 完成标准

Mac 侧完成的最小标准：

- Go 网关可启动，支持四个 HTTP 接口。
- Mac 和 Windows 两个执行端都能主动注册。
- Python Agent dry-run 可完成双端调用。
- OpenAI 兼容 function calling 可完成一次跨机工具循环。
- 最小 3 个工具在 Mac 和 Windows 均可用。
- 超时、未知、幂等、越权四条路径均可观测。
- 审计 JSONL 可追溯且不含秘密。

M5 完成标准：

- 双端 6 个工具可用。
- `darwin`、`windows`、`auto` 三种路由均可演示。
- 断线重连与查询恢复可以复现。
- 面试演示脚本无需手工修改源码即可运行。

## 13. 常见问题与处理

### `go: command not found`

确认 Homebrew 的 bin 路径已经进入 shell 环境：

```bash
brew --prefix
go version
```

### CMake 找不到 Asio 或 nlohmann-json

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

### Windows 无法连接 Mac

先在本机确认端口监听，再从 Windows 检查路由和防火墙。不要通过关闭所有防火墙作为长期方案。

### Agent 收到 `unknown`

使用响应中的 `request_id` 查询，不要重新生成幂等键后直接重提副作用工具。

### token 验签失败

确认网关和两个执行端使用的是同一 `AREC_HMAC_SECRET`，且没有把占位符或多余空格写入环境变量。排障时只报告“验签失败”，不输出 secret。

## 14. Git 与密钥检查

提交前执行：

```bash
git status --short
git diff --cached
```

重点检查：

- 是否有 `.env` 或本地配置被加入。
- 是否误提交 token、API Key、HMAC secret。
- 日志和测试输出是否包含敏感值。
- 示例是否只使用 `YOUR_*_HERE` 或 `<REDACTED>` 占位符。

如果发现真实秘密已经进入工作区或提交历史，立即更换对应秘密并清理历史；不要在回复、日志或文档中复述秘密内容。
