# AREC

AREC（Agent Remote Execution Channel）是面向异构执行端的 Agent 远程工具调用通道。

当前仓库完成了 Windows 数据面的 M1-M3：

- 长度前缀 JSON 帧编解码。
- 网关反向连接、注册、自动重连和状态查询。
- HMAC-SHA256 能力令牌验证。
- `system_info`、`exec_command`、`read_file`。
- 10 分钟 / 1024 条幂等与请求状态缓存。
- Windows Job Object 子进程树终止。
- 单元测试与回环 mock gateway 集成测试。

完整架构见 [ARCHITECTURE.md](ARCHITECTURE.md)，实施规划见 [docs/AREC_PROJECT_PLAN.md](docs/AREC_PROJECT_PLAN.md)。

## 构建

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

## 测试

```powershell
$ctest = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $ctest --test-dir build -C Debug --output-on-failure
```

测试覆盖协议分帧、令牌、幂等、超时、未授权、协议错误，以及断线重连后的状态查询。

## 运行

密钥只能通过环境变量注入，不得写入脚本或仓库：

```powershell
$env:AREC_GATEWAY_DATA_ADDR = '<MAC_LAN_IP>:9000'
$env:AREC_AGENT_ID = 'windows-b'
$env:AREC_HMAC_SECRET = '<REDACTED>'

.\build\Debug\arec-data-plane.exe
```

当前 Windows 端只连接 Go 网关的数据面 TCP 端口。Mac 控制面、Python Agent 和 Mac 数据面按 [docs/AREC_MAC_IMPLEMENTATION.md](docs/AREC_MAC_IMPLEMENTATION.md) 在 Mac 上实施。
