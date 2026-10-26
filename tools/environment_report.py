#!/usr/bin/env python3
"""Emit reproducibility metadata for local benchmark results."""

import argparse
import json
import os
import platform
import shlex
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple


REPOSITORY = Path(__file__).resolve().parents[1]


def command_output(command: List[str], cwd: Optional[Path] = None) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def cpu_model() -> str:
    system = platform.system()
    if system == "Darwin":
        try:
            return command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
        except (OSError, subprocess.CalledProcessError):
            pass
    elif system == "Linux":
        try:
            for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
                field, separator, value = line.partition(":")
                if separator and field.strip() in {"model name", "Hardware"}:
                    return value.strip()
        except OSError:
            pass
    return platform.processor() or platform.machine() or "unknown"


def compiler_version(compiler: str) -> str:
    command = shlex.split(compiler)
    if not command:
        raise ValueError("compiler command is empty")
    output = command_output([*command, "--version"])
    if not output:
        raise ValueError("compiler version output is empty")
    return output.splitlines()[0]


def git_metadata() -> Tuple[str, bool]:
    revision = command_output(["git", "rev-parse", "HEAD"], cwd=REPOSITORY)
    status = command_output(["git", "status", "--porcelain"], cwd=REPOSITORY)
    return revision, not status


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cppflags", default="")
    parser.add_argument("--cxxflags", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    revision, working_tree_clean = git_metadata()
    report = {
        "schemaVersion": 1,
        "operatingSystem": platform.system(),
        "operatingSystemRelease": platform.release(),
        "architecture": platform.machine(),
        "cpuModel": cpu_model(),
        "logicalCpuCount": os.cpu_count(),
        "compilerCommand": args.compiler,
        "compilerVersion": compiler_version(args.compiler),
        "cppFlags": args.cppflags,
        "cxxFlags": args.cxxflags,
        "gitRevision": revision,
        "workingTreeClean": working_tree_clean,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
