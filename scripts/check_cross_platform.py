#!/usr/bin/env python3
"""Verify that the Mac gateway can reach both execution endpoints."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from agent.arec_agent.client import (
    GatewayClient,
    GatewayError,
    IdempotencyRegistry,
)


TOOLS = (
    "system_info",
    "exec_command",
    "read_file",
    "write_file",
    "list_dir",
    "create_dir",
)


def check_cross_platform(
    gateway: GatewayClient,
    *,
    output: Callable[[str], None] = print,
) -> list[dict[str, Any]]:
    session = gateway.create_session(TOOLS)
    token = str(session["token"])
    tools = gateway.list_tools(token)
    platforms: set[str] = set()
    available_names: set[str] = set()
    for tool in tools:
        available_names.add(str(tool.get("name", "")))
        declared = tool.get("platform", [])
        if isinstance(declared, list):
            platforms.update(str(value) for value in declared)

    output(
        json.dumps(
            {
                "event": "tool_registry",
                "tool_count": len(tools),
                "tools": sorted(available_names),
                "platforms": sorted(platforms),
            },
            ensure_ascii=False,
        )
    )
    if platforms != {"darwin", "windows"}:
        raise GatewayError(
            f"both execution endpoints are required; registered platforms: {sorted(platforms)}"
        )
    if available_names != set(TOOLS):
        raise GatewayError(
            f"all six tools are required; registered tools: {sorted(available_names)}"
        )

    registry = IdempotencyRegistry()
    outcomes: list[dict[str, Any]] = []
    for target in ("darwin", "windows", "auto"):
        outcome = gateway.execute_reliably(
            token,
            tool="system_info",
            params={},
            target=target,
            idempotency_key=registry.key_for(f"cross-platform-{target}"),
        )
        response = outcome.response
        if response.get("status") != "ok":
            raise GatewayError(
                f"{target} system_info failed: {response.get('error_code') or response.get('status')}"
            )
        actual_platform = response.get("platform")
        if target != "auto" and actual_platform != target:
            raise GatewayError(
                f"{target} target executed on unexpected platform: {actual_platform}"
            )
        summary = {
            "event": "target_checked",
            "target": target,
            "platform": actual_platform,
            "request_id": response.get("request_id"),
            "status": response.get("status"),
        }
        outcomes.append(summary)
        output(json.dumps(summary, ensure_ascii=False))
    return outcomes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check darwin, windows, and auto routing through an AREC gateway"
    )
    parser.add_argument(
        "--gateway-url",
        default=os.getenv("AREC_GATEWAY_URL", "http://127.0.0.1:8080"),
    )
    arguments = parser.parse_args(argv)

    bootstrap_key = os.getenv("AREC_BOOTSTRAP_KEY", "")
    if not bootstrap_key:
        print("error: AREC_BOOTSTRAP_KEY is required", file=sys.stderr)
        return 2

    gateway = GatewayClient(arguments.gateway_url, bootstrap_key)
    try:
        check_cross_platform(gateway)
    except (GatewayError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
