#!/usr/bin/env python3
"""Run the AREC Mac control-plane and data-plane smoke test."""

from __future__ import annotations

import json
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def http_json(
    method: str,
    url: str,
    *,
    token: str | None = None,
    bootstrap: str | None = None,
    body: dict | None = None,
) -> tuple[int, object]:
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    if bootstrap:
        request.add_header("X-AREC-Bootstrap", bootstrap)
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, json.loads(response.read() or b"null")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"null")


def wait_for_tools(base_url: str, token: str) -> list[dict]:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        status, tools = http_json(
            "GET", f"{base_url}/tools", token=token
        )
        if status == 200 and isinstance(tools, list) and len(tools) == 3:
            return tools
        time.sleep(0.2)
    raise RuntimeError("Mac data plane did not register all three tools")


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main() -> int:
    bootstrap = secrets.token_hex(24)
    hmac_secret = secrets.token_hex(32)
    http_port = free_port()
    data_port = free_port()
    base_url = f"http://127.0.0.1:{http_port}"

    with tempfile.TemporaryDirectory(prefix="arec-smoke-") as directory:
        temp = Path(directory)
        gateway_bin = temp / "arec-gateway"
        audit_path = temp / "audit.jsonl"
        data_plane_bin = ROOT / "build" / "data_plane" / "mac" / "arec-data-plane"
        if not data_plane_bin.exists():
            raise RuntimeError("build the data plane before running the smoke test")

        subprocess.run(
            ["go", "build", "-o", str(gateway_bin), "./cmd/arec-gateway"],
            cwd=ROOT / "gateway",
            check=True,
        )

        environment = os.environ.copy()
        environment.update(
            {
                "AREC_BOOTSTRAP_KEY": bootstrap,
                "AREC_HMAC_SECRET": hmac_secret,
                "AREC_HTTP_ADDR": f"127.0.0.1:{http_port}",
                "AREC_DATA_ADDR": f"127.0.0.1:{data_port}",
                "AREC_AUDIT_PATH": str(audit_path),
            }
        )
        gateway_log = (temp / "gateway.log").open("w", encoding="utf-8")
        gateway = subprocess.Popen(
            [str(gateway_bin)],
            env=environment,
            stdout=gateway_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        data_plane = None
        data_log = None
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    status, _ = http_json("GET", f"{base_url}/healthz")
                    if status == 200:
                        break
                except (OSError, json.JSONDecodeError):
                    pass
                time.sleep(0.1)
            else:
                raise RuntimeError("gateway did not become healthy")

            data_environment = environment.copy()
            data_environment.update(
                {
                    "AREC_GATEWAY_DATA_ADDR": f"127.0.0.1:{data_port}",
                    "AREC_AGENT_ID": "mac-smoke",
                }
            )
            data_log = (temp / "data-plane.log").open("w", encoding="utf-8")
            data_plane = subprocess.Popen(
                [str(data_plane_bin)],
                env=data_environment,
                stdout=data_log,
                stderr=subprocess.STDOUT,
                text=True,
            )

            status, session = http_json(
                "POST",
                f"{base_url}/session",
                bootstrap=bootstrap,
                body={
                    "allowed_tools": ["system_info", "exec_command", "read_file"],
                    "ttl_seconds": 900,
                    "max_calls": 100,
                },
            )
            if status != 200 or not isinstance(session, dict):
                raise RuntimeError(f"session creation failed: {status} {session}")
            token = str(session["token"])
            tools = wait_for_tools(base_url, token)
            if {tool["name"] for tool in tools} != {
                "system_info",
                "exec_command",
                "read_file",
            }:
                raise RuntimeError(f"unexpected tools: {tools}")

            dry_run_environment = environment.copy()
            dry_run_environment["AREC_GATEWAY_URL"] = base_url
            dry_run = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "agent.main",
                    "--dry-run",
                    "--target",
                    "darwin",
                    "--read-file",
                    str(ROOT / "README.md"),
                ],
                cwd=ROOT,
                env=dry_run_environment,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            if dry_run.returncode != 0:
                raise RuntimeError(
                    f"dry-run failed: {dry_run.stdout}\n{dry_run.stderr}"
                )
            summaries = [
                json.loads(line)
                for line in dry_run.stdout.splitlines()
                if line.startswith("{")
            ]
            completed = [
                summary
                for summary in summaries
                if summary.get("tool") and summary.get("status") == "ok"
            ]
            if len(completed) != 3:
                raise RuntimeError(f"dry-run did not complete all tools: {summaries}")

            status, timeout_response = http_json(
                "POST",
                f"{base_url}/execute",
                token=token,
                body={
                    "tool": "exec_command",
                    "params": {"command": "sleep 2"},
                    "target": "darwin",
                    "idempotency_key": secrets.token_hex(16),
                    "deadline_ms": 100,
                },
            )
            if (
                status != 200
                or not isinstance(timeout_response, dict)
                or timeout_response.get("status") != "timeout"
                or timeout_response.get("retryable") is not False
            ):
                raise RuntimeError(
                    f"timeout semantics failed: {status} {timeout_response}"
                )
            request_id = str(timeout_response["request_id"])
            query_status, queried = http_json(
                "GET", f"{base_url}/query/{request_id}", token=token
            )
            if (
                query_status != 200
                or not isinstance(queried, dict)
                or queried.get("status") != "timeout"
            ):
                raise RuntimeError(f"query recovery failed: {query_status} {queried}")

            audit = audit_path.read_text(encoding="utf-8")
            for prohibited in (bootstrap, hmac_secret, token, "sleep 2"):
                if prohibited in audit:
                    raise RuntimeError("audit log contains sensitive or raw data")
            if '"params_hash"' not in audit or '"status":"timeout"' not in audit:
                raise RuntimeError("audit log is missing required reliability fields")
        except Exception:
            gateway_log.flush()
            if data_log is not None:
                data_log.flush()
            print("gateway log:", file=sys.stderr)
            print(Path(gateway_log.name).read_text(encoding="utf-8"), file=sys.stderr)
            if data_log is not None:
                print("data-plane log:", file=sys.stderr)
                print(
                    Path(data_log.name).read_text(encoding="utf-8"),
                    file=sys.stderr,
                )
            raise
        finally:
            if data_plane is not None:
                stop_process(data_plane)
            stop_process(gateway)
            gateway_log.close()
            if data_log is not None:
                data_log.close()

    print("AREC local smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

