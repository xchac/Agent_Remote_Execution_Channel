from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable

import requests


DEFAULT_TIMEOUT_MS = 5000
FINAL_STATUSES = {"ok", "timeout", "failed"}


class GatewayError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        http_status: int | None = None,
        error_code: str | None = None,
        retryable: bool = False,
        request_id: str | None = None,
        response: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.http_status = http_status
        self.error_code = error_code
        self.retryable = retryable
        self.request_id = request_id
        self.response = response or {}


@dataclass
class ExecutionOutcome:
    response: dict[str, Any]
    query_responses: list[dict[str, Any]] = field(default_factory=list)
    rate_limit_retries: int = 0


class GatewayClient:
    def __init__(
        self,
        base_url: str,
        bootstrap_key: str,
        *,
        http: requests.Session | None = None,
        request_timeout: float = 35.0,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self._bootstrap_key = bootstrap_key
        self._http = http or requests.Session()
        self._request_timeout = request_timeout

    def create_session(
        self,
        allowed_tools: Iterable[str],
        *,
        ttl_seconds: int = 900,
        max_calls: int = 100,
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            "/session",
            headers={"X-AREC-Bootstrap": self._bootstrap_key},
            json={
                "allowed_tools": list(allowed_tools),
                "ttl_seconds": ttl_seconds,
                "max_calls": max_calls,
            },
        )

    def list_tools(self, token: str) -> list[dict[str, Any]]:
        result = self._request("GET", "/tools", token=token)
        if not isinstance(result, list):
            raise GatewayError("gateway returned an invalid tools response")
        return result

    def execute(
        self,
        token: str,
        *,
        tool: str,
        params: dict[str, Any],
        target: str,
        idempotency_key: str,
        deadline_ms: int = DEFAULT_TIMEOUT_MS,
    ) -> dict[str, Any]:
        result = self._request(
            "POST",
            "/execute",
            token=token,
            json={
                "tool": tool,
                "params": params,
                "target": target,
                "idempotency_key": idempotency_key,
                "deadline_ms": deadline_ms,
            },
        )
        if not isinstance(result, dict):
            raise GatewayError("gateway returned an invalid execution response")
        return result

    def query(self, token: str, request_id: str) -> dict[str, Any]:
        result = self._request(
            "GET",
            f"/query/{request_id}",
            token=token,
            allow_not_found=True,
        )
        if not isinstance(result, dict):
            raise GatewayError("gateway returned an invalid query response")
        return result

    def execute_reliably(
        self,
        token: str,
        *,
        tool: str,
        params: dict[str, Any],
        target: str,
        idempotency_key: str,
        deadline_ms: int = DEFAULT_TIMEOUT_MS,
        max_rate_limit_retries: int = 3,
        query_delays: tuple[float, ...] = (0.0, 0.25, 0.5, 1.0, 2.0),
        sleep: Callable[[float], None] = time.sleep,
    ) -> ExecutionOutcome:
        response: dict[str, Any]
        rate_limit_retries = 0
        while True:
            try:
                response = self.execute(
                    token,
                    tool=tool,
                    params=params,
                    target=target,
                    idempotency_key=idempotency_key,
                    deadline_ms=deadline_ms,
                )
                break
            except GatewayError as error:
                if error.error_code != "RATE_LIMITED" or rate_limit_retries >= max_rate_limit_retries:
                    raise
                delay = min(0.25 * (2**rate_limit_retries), 2.0)
                sleep(delay)
                rate_limit_retries += 1

        queries: list[dict[str, Any]] = []
        if response.get("status") in {"timeout", "unknown"}:
            request_id = response.get("request_id")
            if not request_id:
                raise GatewayError("gateway omitted request_id for an uncertain call")
            for delay in query_delays:
                if delay > 0:
                    sleep(delay)
                queried = self.query(token, str(request_id))
                queries.append(queried)
                response = queried
                if queried.get("status") in FINAL_STATUSES:
                    break

        return ExecutionOutcome(
            response=response,
            query_responses=queries,
            rate_limit_retries=rate_limit_retries,
        )

    def _request(
        self,
        method: str,
        path: str,
        *,
        token: str | None = None,
        headers: dict[str, str] | None = None,
        json: dict[str, Any] | None = None,
        allow_not_found: bool = False,
    ) -> Any:
        request_headers = {"Content-Type": "application/json"}
        if token:
            request_headers["Authorization"] = f"Bearer {token}"
        if headers:
            request_headers.update(headers)

        try:
            response = self._http.request(
                method,
                self.base_url + path,
                headers=request_headers,
                json=json,
                timeout=self._request_timeout,
            )
        except requests.RequestException as error:
            raise GatewayError(f"gateway request failed: {type(error).__name__}") from error

        try:
            payload = response.json() if response.content else None
        except ValueError as error:
            raise GatewayError(
                "gateway returned non-JSON content",
                http_status=response.status_code,
            ) from error

        if allow_not_found and response.status_code == 404:
            return payload
        if response.status_code < 200 or response.status_code >= 300:
            body = payload if isinstance(payload, dict) else {}
            raise GatewayError(
                str(body.get("message") or f"gateway returned HTTP {response.status_code}"),
                http_status=response.status_code,
                error_code=body.get("error_code"),
                retryable=bool(body.get("retryable", False)),
                request_id=body.get("request_id"),
                response=body,
            )
        return payload


class IdempotencyRegistry:
    def __init__(self, key_factory: Callable[[], str] | None = None) -> None:
        self._key_factory = key_factory or (lambda: str(uuid.uuid4()))
        self._keys: dict[str, str] = {}

    def key_for(self, tool_call_id: str) -> str:
        key = self._keys.get(tool_call_id)
        if key is None:
            key = self._key_factory()
            self._keys[tool_call_id] = key
        return key

