from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Callable

from .client import GatewayClient, GatewayError, IdempotencyRegistry

ALL_TOOLS = (
    "system_info",
    "exec_command",
    "read_file",
    "write_file",
    "list_dir",
    "create_dir",
)


@dataclass
class ModelConfig:
    api_key: str
    base_url: str | None
    model: str


class ToolLoop:
    def __init__(
        self,
        gateway: GatewayClient,
        model_config: ModelConfig,
        *,
        model_client: Any | None = None,
        registry: IdempotencyRegistry | None = None,
        output: Callable[[dict[str, Any]], None] | None = None,
        max_rounds: int = 8,
    ) -> None:
        self._gateway = gateway
        self._model_config = model_config
        self._registry = registry or IdempotencyRegistry()
        self._output = output or (lambda event: None)
        self._max_rounds = max_rounds
        if model_client is None:
            from openai import OpenAI

            model_client = OpenAI(
                api_key=model_config.api_key,
                base_url=model_config.base_url or None,
            )
        self._model_client = model_client

    def run(self, prompt: str, *, target: str = "auto") -> str:
        session = self._gateway.create_session(ALL_TOOLS)
        token = str(session["token"])
        tools = self._gateway.list_tools(token)
        available = {tool["name"]: tool for tool in tools}
        messages: list[dict[str, Any]] = [{"role": "user", "content": prompt}]

        for _ in range(self._max_rounds):
            response = self._model_client.chat.completions.create(
                model=self._model_config.model,
                messages=messages,
                tools=[openai_tool(tool) for tool in tools],
                tool_choice="auto",
            )
            message = as_dict(response.choices[0].message)
            messages.append(message)
            tool_calls = message.get("tool_calls") or []
            if not tool_calls:
                return str(message.get("content") or "")

            for tool_call in tool_calls:
                function = as_dict(tool_call.get("function", {}))
                tool_name = str(function.get("name") or "")
                tool_call_id = str(tool_call.get("id") or "")
                if tool_name not in available:
                    result = {
                        "status": "failed",
                        "error_code": "TOOL_NOT_FOUND",
                        "message": "model requested a tool that is not currently available",
                    }
                else:
                    try:
                        arguments = json.loads(function.get("arguments") or "{}")
                    except json.JSONDecodeError:
                        result = {
                            "status": "failed",
                            "error_code": "INVALID_PARAMS",
                            "message": "model returned invalid JSON arguments",
                        }
                    else:
                        try:
                            outcome = self._gateway.execute_reliably(
                                token,
                                tool=tool_name,
                                params=arguments,
                                target=target,
                                idempotency_key=self._registry.key_for(tool_call_id),
                            )
                        except GatewayError as error:
                            if error.error_code == "UNAUTHORIZED":
                                raise
                            result = {
                                "status": "failed",
                                "error_code": error.error_code,
                                "request_id": error.request_id,
                                "message": str(error),
                            }
                        else:
                            result = outcome.response
                            self._output(
                                {
                                    "tool_call_id": tool_call_id,
                                    "tool": tool_name,
                                    "request_id": result.get("request_id"),
                                    "status": result.get("status"),
                                    "query_count": len(outcome.query_responses),
                                    "rate_limit_retries": outcome.rate_limit_retries,
                                }
                            )
                messages.append(
                    {
                        "role": "tool",
                        "tool_call_id": tool_call_id,
                        "content": json.dumps(result, ensure_ascii=False),
                    }
                )
        raise RuntimeError("model tool loop exceeded the configured round limit")


def openai_tool(tool: dict[str, Any]) -> dict[str, Any]:
    return {
        "type": "function",
        "function": {
            "name": tool["name"],
            "description": tool.get("description", ""),
            "parameters": tool.get("params_schema", {"type": "object", "properties": {}}),
        },
    }


def as_dict(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    if hasattr(value, "model_dump"):
        return value.model_dump(exclude_none=True)
    if hasattr(value, "dict"):
        return value.dict()
    if hasattr(value, "__dict__"):
        return dict(value.__dict__)
    raise TypeError(f"cannot convert model value of type {type(value).__name__}")

