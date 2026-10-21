# AREC M4 跨机联调手册

本文用于在实际 Mac 与 Windows 上完成 M4：

- Mac 运行 Go 网关、Python Agent 和 macOS 数据面。
- Windows 运行数据面并主动连接 Mac。
- Agent 分别通过 `darwin`、`windows`、`auto` 路由执行 `system_info`。

成功标准：

1. 网关 `/tools` 同时看到 `darwin` 和 `windows`。
2. Mac Agent 能在 Mac 和 Windows 各执行一次 `system_info`。
3. `auto` 返回实际执行平台。
4. 审计文件中没有原始 token 或密钥。

## 1. 网络前提

- Mac 和 Windows 连接同一局域网。
- Windows 能访问 Mac 的数据面 TCP 端口，默认 `9000`。
- Mac 的 Agent 只访问本机 HTTP `127.0.0.1:8080`。
- 受控演示环境暂不启用 TLS。

在 Mac 查询局域网地址：

```bash
ipconfig getifaddr en0
```

如果接口不是 `en0`，使用 `ifconfig` 找到实际地址。

## 2. Mac 准备

同步已整合的 `main`：

```bash
git fetch origin
git switch --detach origin/main
```

这里使用 detached HEAD 是为了避开历史重写后旧本地分支与新远端 `main` 的分叉问题。
如果需要在 Mac 上继续提交，再基于当前提交创建新的本地分支：

```bash
git switch -c codex/mac-m4
```

首次实施安装依赖：

```bash
brew install go cmake ninja asio nlohmann-json python@3.12
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r agent/requirements.txt
```

构建并运行本地测试：

```bash
go -C gateway test -race ./...
python -m unittest discover -s agent/tests -v
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## 3. 准备两端一致的秘密

准备两个值：

- `AREC_BOOTSTRAP_KEY`：只给 Mac 网关和 Mac Agent。
- `AREC_HMAC_SECRET`：Mac 网关、Mac 数据面和 Windows 数据面必须完全一致。

建议分别通过安全渠道生成和传递，不要写入仓库、命令历史、聊天记录或截图。

Mac 终端安全输入示例：

```bash
read -r -s AREC_BOOTSTRAP_KEY
export AREC_BOOTSTRAP_KEY
read -r -s AREC_HMAC_SECRET
export AREC_HMAC_SECRET
```

Windows PowerShell 安全输入示例：

```powershell
$secure = Read-Host -AsSecureString
$env:AREC_HMAC_SECRET = [System.Net.NetworkCredential]::new('', $secure).Password
```

## 4. 启动 Mac 网关

数据面地址必须监听局域网可达网卡，不能只绑定 `127.0.0.1`：

```bash
export AREC_HTTP_ADDR="127.0.0.1:8080"
export AREC_DATA_ADDR="0.0.0.0:9000"
export AREC_AUDIT_PATH="var/audit/arec-audit.jsonl"
go -C gateway run ./cmd/arec-gateway
```

保持该终端运行。

## 5. 启动 Mac 数据面

新终端中进入仓库并输入相同的 `AREC_HMAC_SECRET`：

```bash
read -r -s AREC_HMAC_SECRET
export AREC_HMAC_SECRET
export AREC_GATEWAY_DATA_ADDR="127.0.0.1:9000"
export AREC_AGENT_ID="mac-a"
./build/data_plane/mac/arec-data-plane
```

日志应显示已连接并注册，不能持续重连。

## 6. 启动 Windows 数据面

在 Windows PowerShell 中从项目实际路径运行：

```powershell
Set-Location 'F:\Code\Agent_Remote_Execution_Channel'

$secure = Read-Host -AsSecureString
$env:AREC_HMAC_SECRET = [System.Net.NetworkCredential]::new('', $secure).Password

$env:AREC_GATEWAY_DATA_ADDR = '<MAC_LAN_IP>:9000'
$env:AREC_AGENT_ID = 'windows-b'

.\build\data_plane\Debug\arec-data-plane.exe
```

必须把 `<MAC_LAN_IP>` 替换为 Mac 的局域网地址。日志中出现连接和注册信息后保持终端运行。

Windows 防火墙通常不需要开放入站端口，因为 Windows 是主动连接方。

## 7. Mac 上执行跨平台检查

在 Mac 的第三个终端中输入 Bootstrap Key：

```bash
read -r -s AREC_BOOTSTRAP_KEY
export AREC_BOOTSTRAP_KEY
export AREC_GATEWAY_URL="http://127.0.0.1:8080"
python scripts/check_cross_platform.py
```

预期输出包含：

```text
{"event":"tool_registry","tool_count":6,"platforms":["darwin","windows"]}
{"event":"target_checked","target":"darwin","platform":"darwin",...}
{"event":"target_checked","target":"windows","platform":"windows",...}
{"event":"target_checked","target":"auto","platform":"darwin",...}
```

`auto` 的平台可能根据会话亲和性变化，但必须是 `darwin` 或 `windows`，且状态为 `ok`。

## 8. Agent dry-run

Mac 本地目标：

```bash
python -m agent.main --dry-run --target darwin --read-file README.md
```

Windows 目标必须提供 Windows 端可见的文件路径：

```bash
python -m agent.main \
  --dry-run \
  --target windows \
  --remote-read-file 'F:\Code\Agent_Remote_Execution_Channel\README.md'
```

自动路由：

```bash
python -m agent.main --dry-run --target auto
```

三步都必须返回 `status: ok`。如果 Windows 目标的读取路径不存在，只允许 `read_file` 失败，不能影响 `system_info` 和 `exec_command`。

## 9. 接入真实模型

dry-run 通过后，再提供：

```bash
export OPENAI_API_KEY="<REDACTED>"
export OPENAI_MODEL="<MODEL_ID>"
export OPENAI_BASE_URL="<OPENAI_COMPATIBLE_BASE_URL>"
python -m agent.main --target auto
```

密钥只通过环境变量注入。不要打印或提交真实值。

## 10. 审计检查

在 Mac 查看审计 JSONL：

```bash
tail -n 20 var/audit/arec-audit.jsonl
```

确认：

- 同时存在 `darwin` 和 `windows` 调用。
- 存在 `system_info` 的成功记录。
- 有 `params_hash`、`status`、`duration_ms`。
- 没有 Bootstrap Key、HMAC secret、原始 capability token。

不要把整个审计文件粘贴到公开环境；先确认其中没有业务敏感参数。

## 11. 故障排查

Windows 连接被拒绝：

- 检查 Mac 网关是否设置 `AREC_DATA_ADDR="0.0.0.0:9000"`。
- 从 Windows 执行 `Test-NetConnection <MAC_LAN_IP> -Port 9000`。
- 检查 macOS 防火墙是否允许 Go 网关接收连接。

Windows 注册成功但 `/tools` 看不到：

- 检查两端使用完全相同的 `AREC_HMAC_SECRET`。
- 检查网关日志是否出现 `register_ack` 或令牌拒绝。
- 检查 `AREC_AGENT_ID` 是否为空或重复。

`target: windows` 返回 `TOOL_NOT_FOUND`：

- 检查 Windows 数据面是否仍在运行。
- 检查 Windows 是否能持续连接 Mac，而不是反复重连。
- 检查 `/tools` 返回的 `platform` 是否包含 `windows`。

`read_file` 参数错误：

- Windows 目标必须使用 Windows 路径和已存在文件。
- Mac 目标使用 Mac 路径。

## 12. 停止顺序

1. 停止 Mac Agent。
2. 停止 Windows 数据面。
3. 停止 Mac 数据面。
4. 停止 Mac 网关。

随后检查审计文件和工作区状态，确认没有生成未跟踪的密钥或配置文件。
