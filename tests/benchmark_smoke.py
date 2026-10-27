#!/usr/bin/env python3
"""Exercise and validate the PageForge benchmark JSON contract."""

import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path


ROW_COUNT = 200


def fail(message: str) -> None:
    raise RuntimeError(message)


def require_integer(report: dict, field: str, minimum: int = 0) -> int:
    value = report.get(field)
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        fail(f"{field} is not an integer greater than or equal to {minimum}")
    return value


def require_number(report: dict, field: str) -> float:
    value = report.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{field} is not numeric")
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        fail(f"{field} is not finite and positive")
    return number


def validate_throughput(report: dict, duration_field: str, throughput_field: str) -> None:
    duration = require_integer(report, duration_field, minimum=1)
    throughput = require_number(report, throughput_field)
    expected = ROW_COUNT * 1_000_000.0 / duration
    if not math.isclose(throughput, expected, rel_tol=0.0, abs_tol=0.011):
        fail(f"{throughput_field} does not match {duration_field}")


def validate_report(report: dict) -> None:
    expected = {
        "rows": ROW_COUNT,
        "matched_rows": 100,
        "indexed_rows": 1,
        "selected_id_checksum": 9900,
        "indexed_id_checksum": 100,
    }
    for field, value in expected.items():
        if report.get(field) != value:
            fail(f"{field} was {report.get(field)!r}, expected {value!r}")

    require_integer(report, "database_bytes", minimum=1)
    validate_throughput(report, "insert_microseconds", "insert_rows_per_second")
    validate_throughput(report, "select_microseconds", "select_input_rows_per_second")
    validate_throughput(
        report, "index_build_microseconds", "index_build_rows_per_second"
    )
    validate_throughput(
        report, "reopen_scan_microseconds", "reopen_scan_rows_per_second"
    )
    require_integer(report, "reopen_index_lookup_microseconds", minimum=1)


def require_rejected(command: list[str], message: str) -> None:
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode == 0:
        fail(message)


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="pageforge-benchmark-") as temporary_dir:
        database = Path(temporary_dir) / "benchmark.db"
        completed = subprocess.run(
            [binary, str(database), str(ROW_COUNT)],
            check=True,
            capture_output=True,
            text=True,
        )
        validate_report(json.loads(completed.stdout))
        if not database.is_file():
            fail("benchmark did not leave its database for inspection")

        require_rejected(
            [binary, str(database), "10"],
            "benchmark overwrote an existing database",
        )
        zero_database = Path(temporary_dir) / "zero.db"
        require_rejected(
            [binary, str(zero_database), "0"],
            "benchmark accepted a zero row count",
        )
        if zero_database.exists():
            fail("invalid benchmark input created a database")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <pageforge-benchmark-binary>", file=sys.stderr)
        return 2
    try:
        run(sys.argv[1])
    except (json.JSONDecodeError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL  benchmark JSON contract: {error}", file=sys.stderr)
        return 1
    print("PASS  benchmark JSON contract, durability verification, and input validation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
