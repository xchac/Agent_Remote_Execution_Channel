"""AREC Agent command-line entry point."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from agent.arec_agent.client import GatewayClient, GatewayError
from agent.arec_agent.dry_run import run_dry_run
from agent.arec_agent.llm_loop import ModelConfig, ToolLoop


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="AREC OpenAI-compatible tool agent")
    parser.add_argument("--dry-run", action="store_true", help="run the fixed tool-chain check")
    parser.add_argument("--target", choices=("darwin", "windows", "auto"), default="auto")
    parser.add_argument("--read-file", type=Path, default=Path("README.md"))
    parser.add_argument(
        "--prompt",
        default="Inspect the available remote systems, then answer the user's request.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    bootstrap_key = os.getenv("AREC_BOOTSTRAP_KEY", "")
    if not bootstrap_key:
        print("error: AREC_BOOTSTRAP_KEY is required", file=sys.stderr)
        return 2

    gateway = GatewayClient(
        os.getenv("AREC_GATEWAY_URL", "http://127.0.0.1:8080"),
        bootstrap_key,
    )
    try:
        if arguments.dry_run:
            outcomes = run_dry_run(
                gateway,
                read_file=arguments.read_file,
                target=arguments.target,
            )
            return 0 if outcomes else 1

        api_key = os.getenv("OPENAI_API_KEY", "")
        model = os.getenv("OPENAI_MODEL", "")
        if not api_key or not model:
            print("error: OPENAI_API_KEY and OPENAI_MODEL are required", file=sys.stderr)
            return 2
        loop = ToolLoop(
            gateway,
            ModelConfig(
                api_key=api_key,
                base_url=os.getenv("OPENAI_BASE_URL"),
                model=model,
            ),
            output=lambda event: print(json.dumps(event, ensure_ascii=False)),
        )
        answer = loop.run(arguments.prompt, target=arguments.target)
        print(answer)
        return 0
    except (GatewayError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

