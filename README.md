# Agent Remote Execution Channel

AREC is a controlled-environment demonstration of reliable remote tool calls.
The Mac control plane routes OpenAI-compatible tool calls to Mac and Windows
data planes over length-prefixed JSON frames.

## Build

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

## Test

```bash
go -C gateway test -race ./...
python -m unittest discover -s agent/tests -v
ctest --test-dir build --output-on-failure
python scripts/smoke_local.py
```

## Run

Configure secrets only through the process environment. Use placeholders such
as `YOUR_BOOTSTRAP_KEY_HERE` in notes or examples; never add real values to the
repository.

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

In a second terminal, inject the same `AREC_HMAC_SECRET` and run:

```bash
export AREC_GATEWAY_DATA_ADDR="127.0.0.1:9000"
export AREC_AGENT_ID="mac-a"
./build/data_plane/arec-data-plane
```

In a third terminal, inject the same `AREC_BOOTSTRAP_KEY`, activate the virtual
environment, and run the fixed tool-chain check:

```bash
export AREC_GATEWAY_URL="http://127.0.0.1:8080"
python -m agent.main --dry-run --target darwin --read-file README.md
```

Real model mode additionally reads `OPENAI_API_KEY`, `OPENAI_MODEL`, and
optionally `OPENAI_BASE_URL` from the environment.

This first version implements `system_info`, `exec_command`, and `read_file`.
It is not a production remote-execution platform and has no OS sandbox or
transport encryption.
