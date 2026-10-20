from __future__ import annotations

import json
import unittest

from agent.arec_agent.client import (
    ExecutionOutcome,
    GatewayClient,
    GatewayError,
    IdempotencyRegistry,
)


class FakeHTTPResponse:
    def __init__(self, status_code: int, payload=None) -> None:
        self.status_code = status_code
        self._payload = payload
        self.content = b"" if payload is None else json.dumps(payload).encode()

    def json(self):
        return self._payload


class FakeHTTPSession:
    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = []

    def request(self, method, url, **kwargs):
        self.calls.append({"method": method, "url": url, **kwargs})
        return self.responses.pop(0)


class FakeGateway:
    def __init__(self, execute_results=None, query_results=None):
        self.execute_results = list(execute_results or [])
        self.query_results = list(query_results or [])
        self.execute_calls = []
        self.query_calls = []

    def execute(self, token, **kwargs):
        self.execute_calls.append({"token": token, **kwargs})
        result = self.execute_results.pop(0)
        if isinstance(result, Exception):
            raise result
        return result

    def query(self, token, request_id):
        self.query_calls.append({"token": token, "request_id": request_id})
        return self.query_results.pop(0)


class GatewayClientTest(unittest.TestCase):
    def test_create_session_uses_bootstrap_header_without_printing_it(self) -> None:
        http = FakeHTTPSession(
            [
                FakeHTTPResponse(
                    200,
                    {
                        "token": "capability-token",
                        "token_id": "token-1",
                        "session_id": "session-1",
                        "expires_at": "2030-01-01T00:00:00Z",
                    },
                )
            ]
        )
        client = GatewayClient("http://gateway", "bootstrap-secret", http=http)
        created = client.create_session(("system_info",), ttl_seconds=60, max_calls=2)
        self.assertEqual(created["session_id"], "session-1")
        call = http.calls[0]
        self.assertEqual(call["headers"]["X-AREC-Bootstrap"], "bootstrap-secret")
        self.assertNotIn("bootstrap-secret", json.dumps(created))

    def test_execute_reliably_queries_unknown_before_returning(self) -> None:
        gateway = FakeGateway(
            execute_results=[
                {
                    "request_id": "request-1",
                    "status": "unknown",
                    "retryable": False,
                }
            ],
            query_results=[
                {"request_id": "request-1", "status": "running"},
                {"request_id": "request-1", "status": "ok", "result": {"hostname": "mac"}},
            ],
        )
        outcome = GatewayClient.execute_reliably(
            gateway,
            "token",
            tool="system_info",
            params={},
            target="darwin",
            idempotency_key="stable-key",
            query_delays=(0, 0),
            sleep=lambda _: None,
        )
        self.assertIsInstance(outcome, ExecutionOutcome)
        self.assertEqual(outcome.response["status"], "ok")
        self.assertEqual(len(gateway.execute_calls), 1)
        self.assertEqual(len(gateway.query_calls), 2)
        self.assertEqual(gateway.execute_calls[0]["idempotency_key"], "stable-key")

    def test_timeout_is_queried_without_resubmitting_side_effect(self) -> None:
        gateway = FakeGateway(
            execute_results=[
                {
                    "request_id": "request-timeout",
                    "status": "timeout",
                    "error_code": "EXECUTION_TIMEOUT",
                    "retryable": False,
                }
            ],
            query_results=[
                {
                    "request_id": "request-timeout",
                    "status": "timeout",
                    "error_code": "EXECUTION_TIMEOUT",
                    "retryable": False,
                }
            ],
        )
        outcome = GatewayClient.execute_reliably(
            gateway,
            "token",
            tool="exec_command",
            params={"command": "sleep 10"},
            target="darwin",
            idempotency_key="stable-side-effect-key",
            query_delays=(0,),
            sleep=lambda _: None,
        )
        self.assertEqual(outcome.response["status"], "timeout")
        self.assertEqual(len(gateway.execute_calls), 1)
        self.assertEqual(len(gateway.query_calls), 1)

    def test_rate_limit_retries_reuse_idempotency_key(self) -> None:
        rate_error = GatewayError(
            "rate limited",
            http_status=429,
            error_code="RATE_LIMITED",
            retryable=True,
        )
        gateway = FakeGateway(
            execute_results=[
                rate_error,
                rate_error,
                {"request_id": "request-ok", "status": "ok", "result": {}},
            ]
        )
        outcome = GatewayClient.execute_reliably(
            gateway,
            "token",
            tool="system_info",
            params={},
            target="auto",
            idempotency_key="rate-limit-key",
            sleep=lambda _: None,
        )
        self.assertEqual(outcome.rate_limit_retries, 2)
        self.assertEqual(
            [call["idempotency_key"] for call in gateway.execute_calls],
            ["rate-limit-key", "rate-limit-key", "rate-limit-key"],
        )

    def test_query_not_found_is_returned_as_unknown(self) -> None:
        http = FakeHTTPSession(
            [
                FakeHTTPResponse(
                    404,
                    {
                        "request_id": "missing",
                        "status": "unknown",
                        "error_code": None,
                        "retryable": False,
                    },
                )
            ]
        )
        client = GatewayClient("http://gateway", "bootstrap", http=http)
        response = client.query("token", "missing")
        self.assertEqual(response["status"], "unknown")

    def test_idempotency_registry_is_stable(self) -> None:
        values = iter(("first", "second"))
        registry = IdempotencyRegistry(lambda: next(values))
        self.assertEqual(registry.key_for("call-1"), "first")
        self.assertEqual(registry.key_for("call-1"), "first")
        self.assertEqual(registry.key_for("call-2"), "second")


if __name__ == "__main__":
    unittest.main()

