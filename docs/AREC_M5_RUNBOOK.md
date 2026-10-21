# AREC M5 六工具验收手册

本文用于验证双端完整 MVP：

- `system_info`
- `exec_command`
- `read_file`
- `write_file`
- `list_dir`
- `create_dir`

成功标准：

1. `/tools` 同时包含六个工具和 `darwin`、`windows` 平台。
2. Mac 与 Windows 都能完成 `create_dir`、`write_file`、`list_dir`、`read_file`。
3. 重复 `create_dir` 返回 `created=false`。
4. 相同幂等键重复 `write_file` 返回一致结果。
5. 审计不包含原始路径、文件内容或 token。

## 1. 同步与构建

Mac：

```bash
git fetch origin
git switch --detach origin/main
go -C gateway test ./...
python -m unittest discover -s agent/tests -v
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --build 'F:\Code\Agent_Remote_Execution_Channel\build' --config Debug --parallel

$ctest = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $ctest --test-dir 'F:\Code\Agent_Remote_Execution_Channel\build' -C Debug --output-on-failure
```

## 2. 启动两端

使用新的随机 HMAC Secret，停止并重启 Mac 网关、Mac 数据面和 Windows 数据面。

Mac 网关：

```bash
export AREC_HTTP_ADDR="127.0.0.1:8080"
export AREC_DATA_ADDR="0.0.0.0:9000"
export AREC_AUDIT_PATH="var/audit/arec-audit.jsonl"
go -C gateway run ./cmd/arec-gateway
```

Mac 数据面：

```bash
export AREC_GATEWAY_DATA_ADDR="127.0.0.1:9000"
export AREC_AGENT_ID="mac-a"
./build/data_plane/mac/arec-data-plane
```

Windows 数据面：

```powershell
Set-Location 'F:\Code\Agent_Remote_Execution_Channel'
$env:AREC_GATEWAY_DATA_ADDR = '<MAC_LAN_IP>:9000'
$env:AREC_AGENT_ID = 'windows-b'
.\build\data_plane\Debug\arec-data-plane.exe
```

秘密由两端在本机安全输入，不进入命令历史、仓库或聊天。

## 3. 平台检查

在 Mac 执行：

```bash
export AREC_GATEWAY_URL="http://127.0.0.1:8080"
python scripts/check_cross_platform.py
```

必须看到：

```text
{"event":"tool_registry","tool_count":6,"tools":[...],"platforms":["darwin","windows"]}
{"event":"target_checked","target":"darwin","platform":"darwin",...}
{"event":"target_checked","target":"windows","platform":"windows",...}
{"event":"target_checked","target":"auto","platform":"darwin",...}
```

## 4. 六工具执行

在 Mac 执行：

```bash
python scripts/check_m5_tools.py \
  --darwin-root /tmp/arec-m5-checks \
  --windows-root 'F:\Code\Agent_Remote_Execution_Channel\out\m5-checks'
```

脚本会对两个平台分别：

1. `create_dir` 创建嵌套目录。
2. 再次 `create_dir`，验证幂等。
3. `write_file` 写入测试内容。
4. 使用同一幂等键重复 `write_file`。
5. `list_dir` 验证文件存在。
6. `read_file` 验证内容一致。

每个平台应输出一条 `m5_target_checked`，且：

- `create_idempotent=true`
- `write_idempotent=true`
- `list_entries` 至少为 1
- `read_bytes` 大于 0

## 5. 审计检查

```bash
tail -n 100 var/audit/arec-audit.jsonl
```

确认：

- 同时存在 `darwin` 和 `windows`。
- 六个工具都出现在审计中。
- `write_file` 审计只包含 `params_hash`，不包含文件内容或原始路径。
- 不包含 Bootstrap Key、HMAC Secret 或 capability token。

## 6. 失败排查

`/tools` 只有三工具：

- 某一端执行的仍是旧二进制。
- 重新构建并重启对应数据面。

`write_file` 返回 `EXECUTION_FAILED`：

- 检查父目录是否存在。
- Windows 路径必须是 `F:\...` 形式。
- Mac 路径必须是 `/tmp/...` 或其他实际存在的父目录。

`list_dir` 返回 `truncated=true`：

- 目录条目超过 256 个，属于预期保护。

`create_dir` 重复调用 `created=true`：

- 两次调用使用了不同路径，或旧数据面没有更新。

## 7. 停止

依次停止 Mac Agent、Windows 数据面、Mac 数据面和 Mac 网关。

确认 Windows 无 `arec-data-plane` 进程，Mac 的 `8080`、`9000` 已释放。
