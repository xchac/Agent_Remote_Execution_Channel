from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from agent.arec_agent.client import ExecutionOutcome
from agent.arec_agent.dry_run import run_dry_run


class FakeGateway:
    def __init__(self, available_tools) -> None:
        self.available_tools = available_tools
        self.calls = []

    def create_session(self, allowed_tools):
        self.calls.append(("create_session", tuple(allowed_tools)))
        return {"token": "secret-token", "session_id": "session-1"}

    def list_tools(self, token):
        self.calls.append(("list_tools", token))
        return [{"name": name} for name in self.available_tools]

    def execute_reliably(self, token, **kwargs):
        self.calls.append(("execute", token, kwargs))
        return ExecutionOutcome(
            response={
                "request_id": f"request-{len(self.calls)}",
                "status": "ok",
                "platform": "darwin",
            }
        )


class DryRunTest(unittest.TestCase):
    def test_fixed_sequence_and_output_redaction(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.txt"
            path.write_text("hello", encoding="utf-8")
            gateway = FakeGateway(("system_info", "exec_command", "read_file"))
            lines = []
            outcomes = run_dry_run(
                gateway,
                read_file=path,
                target="darwin",
                output=lines.append,
            )
        self.assertEqual(len(outcomes), 3)
        tool_calls = [call for call in gateway.calls if call[0] == "execute"]
        self.assertEqual(
            [call[2]["tool"] for call in tool_calls],
            ["system_info", "exec_command", "read_file"],
        )
        self.assertTrue(all(call[2]["target"] == "darwin" for call in tool_calls))
        self.assertNotIn("secret-token", "\n".join(lines))

    def test_unavailable_tool_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.txt"
            path.write_text("hello", encoding="utf-8")
            gateway = FakeGateway(("system_info",))
            outcomes = run_dry_run(
                gateway,
                read_file=path,
                output=lambda _: None,
            )
        self.assertEqual([step["status"] for step in outcomes], ["ok", "skipped", "skipped"])

    def test_windows_plan_uses_windows_command_and_remote_path(self) -> None:
        gateway = FakeGateway(("system_info", "exec_command", "read_file"))
        remote_path = r"F:\Code\Agent_Remote_Execution_Channel\README.md"
        outcomes = run_dry_run(
            gateway,
            read_file=Path("unused-local-path"),
            remote_read_file=remote_path,
            target="windows",
            output=lambda _: None,
        )

        self.assertEqual(len(outcomes), 3)
        tool_calls = [call for call in gateway.calls if call[0] == "execute"]
        self.assertEqual(tool_calls[1][2]["params"]["command"], "ver")
        self.assertEqual(tool_calls[2][2]["params"]["path"], remote_path)

    def test_windows_plan_requires_remote_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.txt"
            path.write_text("hello", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "remote_read_file"):
                run_dry_run(
                    FakeGateway(("system_info",)),
                    read_file=path,
                    target="windows",
                    output=lambda _: None,
                )


if __name__ == "__main__":
    unittest.main()

