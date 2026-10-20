# Agent Remote Execution Channel

AREC（Agent Remote Execution Channel）是面向异构执行端的远程工具调用通道。
Mac 控制面通过 OpenAI 兼容 function calling 发起调用，经 Go 网关路由到
macOS 或 Windows 数据面。数据面使用 4 字节大端长度前缀的 JSON 帧主动连接网关。

当前实现包含：

- Go 控制面网关、Python Agent 和 macOS 数据面。
- Windows M1-M3 数据面、Job Object 进程树终止及 mock gateway 测试。
- `system_info`、`exec_command`、`read_file`，三目标路由、幂等、限流和 JSONL 审计。

实际双机联调步骤见 [docs/AREC_M4_RUNBOOK.md](docs/AREC_M4_RUNBOOK.md)。

## macOS 构建

```bash
go -C gateway mod download
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install -r agent/requirements.txt

cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## macOS 测试

```bash
go -C gateway test -race ./...
python -m unittest discover -s agent/tests -v
ctest --test-dir build --output-on-failure
python scripts/smoke_local.py
```

## macOS 运行

密钥只能通过环境变量注入。示例只使用占位符，不得写入真实值。

```bash
read -r -s AREC_BOOTSTRAP_KEY
export AREC_BOOTSTRAP_KEY
read -r -s AREC_HMAC_SECRET
export AREC_HMAC_SECRET
export AREC_HTTP_ADDR=":8080"
export AREC_DATA_ADDR=":9000"
export AREC_AUDIT_PATH="var/audit/arec-audit.jsonl"
go -C gateway run ./cmd/arec-gateway
```

第二个终端注入相同的 `AREC_HMAC_SECRET`：

```bash
export AREC_GATEWAY_DATA_ADDR="127.0.0.1:9000"
export AREC_AGENT_ID="mac-a"
./build/data_plane/mac/arec-data-plane
```

第三个终端注入相同的 `AREC_BOOTSTRAP_KEY`：

```bash
export AREC_GATEWAY_URL="http://127.0.0.1:8080"
python -m agent.main --dry-run --target darwin --read-file README.md
```

真实模型模式另外从环境读取 `OPENAI_API_KEY`、`OPENAI_MODEL` 和可选的
`OPENAI_BASE_URL`。

## Windows 构建

使用 VS 2022 Developer PowerShell，或直接调用 VS 自带 CMake：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$toolchain = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\scripts\buildsystems\vcpkg.cmake'

& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DBUILD_TESTING=ON

& $cmake --build build --config Debug --parallel
```

vcpkg 依赖由 `vcpkg.json` 管理。

## Windows 测试

```powershell
$ctest = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $ctest --test-dir build -C Debug --output-on-failure
```

## Windows 运行

```powershell
$env:AREC_GATEWAY_DATA_ADDR = '<MAC_LAN_IP>:9000'
$env:AREC_AGENT_ID = 'windows-b'
$env:AREC_HMAC_SECRET = '<REDACTED>'

.\build\Debug\arec-data-plane.exe
```

首版仍是受控演示环境，不包含 OS 沙箱或数据面传输加密。
