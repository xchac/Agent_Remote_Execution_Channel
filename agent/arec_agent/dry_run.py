from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Callable, Iterable

from .client import DEFAULT_TIMEOUT_MS, GatewayClient, IdempotencyRegistry


DEFAULT_TOOLS = ("system_info", "exec_command", "read_file")


def run_dry_run(
    gateway: GatewayClient,
    *,
    read_file: Path,
    target: str = "darwin",
    output: Callable[[str], None] = print,
    registry: IdempotencyRegistry | None = None,
) -> list[dict[str, Any]]:
    resolved_file = read_file.resolve()
    if not resolved_file.exists():
        raise ValueError(f"dry-run file does not exist: {resolved_file}")

    session = gateway.create_session(DEFAULT_TOOLS)
    token = str(session["token"])
    available = {tool["name"] for tool in gateway.list_tools(token)}
    plan: list[tuple[str, dict[str, Any], str]] = [
        ("system_info", {}, target),
        ("exec_command", {"command": "uname -s"}, target),
        (
            "read_file",
            {"path": str(resolved_file), "max_bytes": 65536},
            target,
        ),
    ]
    registry = registry or IdempotencyRegistry()
    outcomes: list[dict[str, Any]] = []

    output(
        json.dumps(
            {
                "event": "session_created",
                "session_id": session.get("session_id"),
                "available_tools": sorted(available),
            },
            ensure_ascii=False,
        )
    )
    for index, (tool, params, call_target) in enumerate(plan, start=1):
        if tool not in available:
            summary = {
                "step": index,
                "tool": tool,
                "status": "skipped",
                "reason": "tool is not currently available",
            }
            outcomes.append(summary)
            output(json.dumps(summary, ensure_ascii=False))
            continue

        outcome = gateway.execute_reliably(
            token,
            tool=tool,
            params=params,
            target=call_target,
            idempotency_key=registry.key_for(f"dry-run-{index}-{tool}"),
            deadline_ms=DEFAULT_TIMEOUT_MS,
        )
        summary = {
            "step": index,
            "tool": tool,
            "request_id": outcome.response.get("request_id"),
            "status": outcome.response.get("status"),
            "platform": outcome.response.get("platform"),
            "query_count": len(outcome.query_responses),
            "rate_limit_retries": outcome.rate_limit_retries,
        }
        outcomes.append(summary)
        output(json.dumps(summary, ensure_ascii=False))
    return outcomes

