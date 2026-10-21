#!/usr/bin/env python3
"""Exercise all six AREC tools on macOS and Windows."""

from __future__ import annotations

import argparse
import json
import os
import sys
import uuid
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from agent.arec_agent.client import (
    GatewayClient,
    GatewayError,
    IdempotencyRegistry,
)


ALL_TOOLS = (
    "system_info",
    "exec_command",
    "read_file",
    "write_file",
    "list_dir",
    "create_dir",
)
DEFAULT_WINDOWS_ROOT = r"F:\Code\Agent_Remote_Execution_Channel\out\m5-checks"
DEFAULT_DARWIN_ROOT = "/tmp/arec-m5-checks"


def execute_checked(
    gateway: GatewayClient,
    token: str,
    registry: IdempotencyRegistry,
    *,
    target: str,
    tool: str,
    params: dict[str, Any],
    key_id: str,
) -> dict[str, Any]:
    outcome = gateway.execute_reliably(
        token,
        tool=tool,
        params=params,
        target=target,
        idempotency_key=registry.key_for(key_id),
    )
    response = outcome.response
    if response.get("status") != "ok":
        raise GatewayError(
            f"{target}/{tool} failed: {response.get('error_code') or response.get('status')}"
        )
    if response.get("platform") != target:
        raise GatewayError(
            f"{target}/{tool} ran on unexpected platform: {response.get('platform')}"
        )
    return response


def check_target(
    gateway: GatewayClient,
    token: str,
    registry: IdempotencyRegistry,
    *,
    target: str,
    root: str,
    run_id: str,
) -> dict[str, Any]:
    root_path = PureWindowsPath(root) if target == "windows" else PurePosixPath(root)
    test_root = str(root_path / f"m5-{run_id}-{target}")
    nested = str(PureWindowsPath(test_root) / "nested") if target == "windows" else str(
        PurePosixPath(test_root) / "nested"
    )
    file_path = str(PureWindowsPath(nested) / "proof.txt") if target == "windows" else str(
        PurePosixPath(nested) / "proof.txt"
    )
    content = f"AREC M5 proof {run_id} {target}"

    create_response = execute_checked(
        gateway,
        token,
        registry,
        target=target,
        tool="create_dir",
        params={"path": nested},
        key_id=f"m5-{target}-create",
    )
    create_again = execute_checked(
        gateway,
        token,
        registry,
        target=target,
        tool="create_dir",
        params={"path": nested},
        key_id=f"m5-{target}-create-again",
    )
    write_response = execute_checked(
        gateway,
        token,
        registry,
        target=target,
        tool="write_file",
        params={"path": file_path, "content": content},
        key_id=f"m5-{target}-write",
    )
    write_again = execute_checked(
        gateway,
        token,
        registry,
        target=target,
        tool="write_file",
        params={"path": file_path, "content": content},
        key_id=f"m5-{target}-write",
    )
    list_response = execute_checked(
        gateway,
        token,
        registry,
        target=target,
        tool="list_dir",
        params={"path": nested},
        key_id=f"m5-{target}-list",
    )
    read_response = execute_checked(
        gateway,
        token,
        registry,
        target=target,
        tool="read_file",
        params={"path": file_path, "max_bytes": 65536},
        key_id=f"m5-{target}-read",
    )

    entries = list_response.get("result", {}).get("entries", [])
    if not any(
        entry.get("name") == "proof.txt" and entry.get("type") == "file"
        for entry in entries
    ):
        raise GatewayError(f"{target}/list_dir did not return proof.txt")
    if read_response.get("result", {}).get("content") != content:
        raise GatewayError(f"{target}/read_file returned unexpected content")
    if create_again.get("result", {}).get("created") is not False:
        raise GatewayError(f"{target}/create_dir was not idempotent")
    if write_again.get("result") != write_response.get("result"):
        raise GatewayError(f"{target}/write_file idempotency result changed")

    return {
        "event": "m5_target_checked",
        "target": target,
        "path": nested,
        "created": create_response.get("result", {}).get("created"),
        "create_idempotent": True,
        "write_bytes": write_response.get("result", {}).get("bytes_written"),
        "write_idempotent": True,
        "list_entries": len(entries),
        "read_bytes": read_response.get("result", {}).get("bytes_read"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Exercise all six AREC tools")
    parser.add_argument(
        "--gateway-url",
        default=os.getenv("AREC_GATEWAY_URL", "http://127.0.0.1:8080"),
    )
    parser.add_argument("--darwin-root", default=DEFAULT_DARWIN_ROOT)
    parser.add_argument("--windows-root", default=DEFAULT_WINDOWS_ROOT)
    arguments = parser.parse_args(argv)

    bootstrap_key = os.getenv("AREC_BOOTSTRAP_KEY", "")
    if not bootstrap_key:
        print("error: AREC_BOOTSTRAP_KEY is required", file=sys.stderr)
        return 2

    gateway = GatewayClient(arguments.gateway_url, bootstrap_key)
    registry = IdempotencyRegistry()
    run_id = uuid.uuid4().hex[:12]
    try:
        session = gateway.create_session(ALL_TOOLS)
        token = str(session["token"])
        tools = gateway.list_tools(token)
        if {tool.get("name") for tool in tools} != set(ALL_TOOLS):
            raise GatewayError("all six tools must be registered on both platforms")
        summaries = [
            check_target(
                gateway,
                token,
                registry,
                target="darwin",
                root=arguments.darwin_root,
                run_id=run_id,
            ),
            check_target(
                gateway,
                token,
                registry,
                target="windows",
                root=arguments.windows_root,
                run_id=run_id,
            ),
        ]
        for summary in summaries:
            print(json.dumps(summary, ensure_ascii=False))
    except (GatewayError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
