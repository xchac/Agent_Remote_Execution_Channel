from __future__ import annotations

import unittest

from agent.arec_agent.client import ExecutionOutcome, GatewayError
from agent.arec_agent.llm_loop import ModelConfig, ToolLoop


class FakeGateway:
    def __init__(self, unauthorized=False) -> None:
        self.unauthorized = unauthorized
        self.execute_calls = []

    def create_session(self, allowed_tools):
        return {"token": "secret-token", "session_id": "session-1"}

    def list_tools(self, token):
        return [
            {
                "name": "system_info",
                "description": "system info",
                "params_schema": {"type": "object", "properties": {}},
            }
        ]

    def execute_reliably(self, token, **kwargs):
        self.execute_calls.append(kwargs)
        if self.unauthorized:
            raise GatewayError(
                "unauthorized",
                http_status=401,
                error_code="UNAUTHORIZED",
            )
        return ExecutionOutcome(
            response={
                "request_id": "request-1",
                "status": "ok",
                "result": {"hostname": "mac", "os": "macOS", "arch": "arm64"},
            }
        )


class FakeCompletions:
    def __init__(self, messages):
        self.messages = list(messages)
        self.calls = []

    def create(self, **kwargs):
        self.calls.append(kwargs)
        message = self.messages.pop(0)

        class Response:
            choices = [type("Choice", (), {"message": message})()]

        return Response()


class FakeModel:
    def __init__(self, messages):
        self.chat = type("Chat", (), {"completions": FakeCompletions(messages)})()


class ToolLoopTest(unittest.TestCase):
    def test_tool_call_and_final_answer(self) -> None:
        model = FakeModel(
            [
                {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": "call-1",
                            "type": "function",
                            "function": {"name": "system_info", "arguments": "{}"},
                        }
                    ],
                },
                {"role": "assistant", "content": "Mac is ready."},
            ]
        )
        gateway = FakeGateway()
        events = []
        loop = ToolLoop(
            gateway,
            ModelConfig(api_key="fake", base_url=None, model="fake-model"),
            model_client=model,
            output=events.append,
        )
        answer = loop.run("check the Mac", target="darwin")
        self.assertEqual(answer, "Mac is ready.")
        self.assertEqual(len(gateway.execute_calls), 1)
        self.assertTrue(gateway.execute_calls[0]["idempotency_key"])
        self.assertEqual(events[0]["request_id"], "request-1")

    def test_unauthorized_terminates_loop(self) -> None:
        model = FakeModel(
            [
                {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": "call-unauthorized",
                            "type": "function",
                            "function": {"name": "system_info", "arguments": "{}"},
                        }
                    ],
                }
            ]
        )
        loop = ToolLoop(
            FakeGateway(unauthorized=True),
            ModelConfig(api_key="fake", base_url=None, model="fake-model"),
            model_client=model,
        )
        with self.assertRaises(GatewayError):
            loop.run("check", target="auto")

    def test_same_tool_call_id_reuses_idempotency_key(self) -> None:
        model = FakeModel(
            [
                {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": "stable-call",
                            "type": "function",
                            "function": {"name": "system_info", "arguments": "{}"},
                        },
                        {
                            "id": "stable-call",
                            "type": "function",
                            "function": {"name": "system_info", "arguments": "{}"},
                        },
                    ],
                },
                {"role": "assistant", "content": "done"},
            ]
        )
        gateway = FakeGateway()
        loop = ToolLoop(
            gateway,
            ModelConfig(api_key="fake", base_url=None, model="fake-model"),
            model_client=model,
        )
        loop.run("check", target="darwin")
        keys = [call["idempotency_key"] for call in gateway.execute_calls]
        self.assertEqual(keys[0], keys[1])


if __name__ == "__main__":
    unittest.main()

