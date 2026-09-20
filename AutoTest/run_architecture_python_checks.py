#!/usr/bin/env python3
"""Targeted architecture Python checks entry (delegation plan 2026-09-16, M1).

Runs an explicit, frozen whitelist of pure-Python static and synthetic tests
under AutoTest/. Standard library only. This entry is a targeted selector, not
a regression suite and not an auto-discovery runner.

Contract:
- --group recorder|render-host|all (default all). Unknown groups are rejected
  by the CLI. Arbitrary script names, globs and auto-discovery are forbidden;
  the entry's own test is never added to the whitelist (no recursion).
- --list only prints the selection. It never imports or executes tests and
  never claims that anything was verified.
- --output NEW_JSON is optional. If the target already exists, the run is
  refused before any test executes and the existing bytes are never touched.
  The file is created with CreateNew ("x") semantics. A publication failure
  is folded into the final machine-readable result: ok=false with an explicit
  output error and a non-zero exit code, and the final stdout JSON is printed
  only after the publication attempt. Without --output the summary JSON goes
  to stdout only. With --output the summary is written to the new file and
  the same final payload is printed to stdout.
- The tree root is derived from this file's location, never from the launch
  cwd. Each test runs as ``sys.executable -B <absolute test path>`` with cwd
  pinned to the tree root, without a shell, strictly serially, with a fixed
  120 second timeout per script. On Windows subprocesses receive
  CREATE_NO_WINDOW and BELOW_NORMAL_PRIORITY_CLASS.
- Before any launch the whole selection is checked for duplicates and file
  existence. Each script records pre/post size and SHA-256, returncode,
  timeout/launch error, stdout and stderr (recorded as text whose UTF-8
  encoding is at most 64 KiB per stream, with an explicit truncation flag).
  The 64 KiB value only bounds what is recorded in the summary; subprocess
  output capture itself is not a hard memory limit and is not process-level
  memory protection. A failing script never stops later whitelist scripts;
  results are collected for the whole selection.
- ok=false / non-zero exit on any failure, timeout, launch error, source
  identity change, output publication failure or zero executions. Real
  failures of existing tests are reported as they are; this entry never
  modifies assertions, thresholds or skips. Counts are script counts, not
  assertion counts.
- SHA-256 values identify only this entry and the selected whitelist scripts
  at capture time; they do not claim coverage of indirect dependencies. No
  native build, native run_* gate, GPU, game, DLL build or deployment happens
  here; existing native gates remain independent and their mains are never
  invoked by this entry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
AUTO_TEST_DIR = ROOT / "AutoTest"
ENTRY_RELATIVE_PATH = "AutoTest/run_architecture_python_checks.py"

SCHEMA_VERSION = 1
SCOPE = "TARGETED_PYTHON_STATIC_AND_SYNTHETIC_ONLY"
SCRIPT_TIMEOUT_SECONDS = 120
MAX_STREAM_BYTES = 64 * 1024

RECORDER_TESTS = (
    "test_analyze_frame_evidence.py",
    "test_analyze_frame_history.py",
    "test_frame_inputs.py",
    "test_frame_evidence_control_static.py",
    "test_frame_history_static.py",
    "test_frame_history_shortcut_static.py",
    "test_frame_recorder_self_contained_static.py",
    "test_frame_recorder_memory_static.py",
    "test_frame_recorder_default_policy_static.py",
    "test_self_contained_recorder_gate.py",
    "test_recorder_control_lease_gate.py",
)

RENDER_HOST_TESTS = (
    "test_render_host_transport_gate.py",
    "test_render_host_shared_slots_gate.py",
    "test_render_host_producer_inbox_gate.py",
    "test_render_host_sample_worker_gate.py",
)

# "all" is exactly the recorder list followed by the render-host list, in the
# frozen order above; nothing else is ever added here.
GROUPS = {
    "recorder": RECORDER_TESTS,
    "render-host": RENDER_HOST_TESTS,
    "all": RECORDER_TESTS + RENDER_HOST_TESTS,
}

GROUP_CHOICES = ("recorder", "render-host", "all")

NOT_RUN = ("native", "gpu", "game", "dll_build", "deployment")

if sys.platform == "win32":
    _CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
    _BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
    CREATION_FLAGS = _CREATE_NO_WINDOW | _BELOW_NORMAL_PRIORITY_CLASS
else:
    CREATION_FLAGS = 0


def select_group(group: str) -> tuple[str, ...]:
    """Return the frozen whitelist for an explicit group name."""
    try:
        return GROUPS[group]
    except KeyError:
        raise ValueError(
            "unknown group: %r; expected one of %s" % (group, ", ".join(GROUP_CHOICES))
        ) from None


def selection_paths(group: str) -> tuple[Path, ...]:
    """Resolve the frozen whitelist to absolute AutoTest paths."""
    return tuple((AUTO_TEST_DIR / name) for name in select_group(group))


def display_path(path: Path) -> str:
    """Stable display path relative to the tree root when possible."""
    resolved = Path(path).resolve()
    try:
        return resolved.relative_to(ROOT).as_posix()
    except ValueError:
        return resolved.as_posix()


def file_identity(path: Path) -> dict[str, Any] | None:
    """Size and SHA-256 of a file, or None when it cannot be read."""
    path = Path(path)
    try:
        size = path.stat().st_size
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return {"size": size, "sha256": digest.hexdigest()}


def verify_selection(paths) -> list[dict[str, str]]:
    """Pre-flight check: no duplicates and every file exists."""
    problems: list[dict[str, str]] = []
    seen: set[str] = set()
    for path in paths:
        key = display_path(Path(path))
        if key in seen:
            problems.append({"kind": "duplicate", "path": key})
        else:
            seen.add(key)
        if not Path(path).is_file():
            problems.append({"kind": "missing", "path": key})
    return problems


def clip_stream(data: Any) -> tuple[str, bool, int]:
    """Decode captured output, clipped to MAX_STREAM_BYTES with a flag.

    The returned text is valid Unicode whose UTF-8 encoding is at most
    MAX_STREAM_BYTES bytes. Replacement decoding can expand (every invalid
    input byte becomes U+FFFD, three bytes in UTF-8), so after decoding, the
    text is trimmed by whole characters from the end until its UTF-8 encoding
    fits the cap; whenever any content is dropped the truncation flag is
    True. The third return value always preserves the original raw byte
    count.

    This clipping only bounds what is recorded in the summary. It is not a
    subprocess memory limit and not process-level memory protection: the
    subprocess pipes themselves can buffer more than this before the process
    exits or times out.
    """
    if data is None:
        return "", False, 0
    if isinstance(data, str):
        data = data.encode("utf-8", "replace")
    raw_bytes = len(data)
    truncated = raw_bytes > MAX_STREAM_BYTES
    text = data[:MAX_STREAM_BYTES].decode("utf-8", "replace")
    if len(text.encode("utf-8")) > MAX_STREAM_BYTES:
        # Monotone prefix search for the longest character prefix whose
        # UTF-8 encoding still fits the recorded-stream cap.
        lo, hi = 0, len(text)
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if len(text[:mid].encode("utf-8")) <= MAX_STREAM_BYTES:
                lo = mid
            else:
                hi = mid - 1
        text = text[:lo]
        truncated = True
    return text, truncated, raw_bytes

def record_passed(record: dict[str, Any]) -> bool:
    return (
        record["executed"]
        and record["returncode"] == 0
        and not record["timed_out"]
        and record["launch_error"] is None
        and not record["identity_changed"]
    )


def run_one(test_path: Path, timeout: int = SCRIPT_TIMEOUT_SECONDS) -> dict[str, Any]:
    """Run a single whitelist script and capture its full evidence record."""
    test_path = Path(test_path).resolve()
    command = [sys.executable, "-B", str(test_path)]
    record: dict[str, Any] = {
        "path": display_path(test_path),
        "command": command,
        "cwd": str(ROOT),
        "timeout_seconds": timeout,
        "size_before": None,
        "sha256_before": None,
        "size_after": None,
        "sha256_after": None,
        "identity_changed": False,
        "attempted": False,
        "executed": False,
        "returncode": None,
        "timed_out": False,
        "launch_error": None,
        "duration_seconds": None,
        "stdout": "",
        "stdout_bytes": 0,
        "stdout_truncated": False,
        "stderr": "",
        "stderr_bytes": 0,
        "stderr_truncated": False,
    }
    before = file_identity(test_path)
    if before is None:
        record["launch_error"] = "source file unreadable or missing before launch"
        return record
    record["size_before"] = before["size"]
    record["sha256_before"] = before["sha256"]
    record["attempted"] = True
    started_at = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            cwd=str(ROOT),
            shell=False,
            capture_output=True,
            timeout=timeout,
            creationflags=CREATION_FLAGS,
        )
    except subprocess.TimeoutExpired as exc:
        record["timed_out"] = True
        record["executed"] = True
        (
            record["stdout"],
            record["stdout_truncated"],
            record["stdout_bytes"],
        ) = clip_stream(getattr(exc, "stdout", None))
        (
            record["stderr"],
            record["stderr_truncated"],
            record["stderr_bytes"],
        ) = clip_stream(getattr(exc, "stderr", None))
    except OSError as exc:
        record["launch_error"] = "%s: %s" % (type(exc).__name__, exc)
    else:
        record["executed"] = True
        record["returncode"] = completed.returncode
        (
            record["stdout"],
            record["stdout_truncated"],
            record["stdout_bytes"],
        ) = clip_stream(completed.stdout)
        (
            record["stderr"],
            record["stderr_truncated"],
            record["stderr_bytes"],
        ) = clip_stream(completed.stderr)
    finally:
        record["duration_seconds"] = round(time.monotonic() - started_at, 6)
    after = file_identity(test_path)
    if after is None:
        record["identity_changed"] = True
    else:
        record["size_after"] = after["size"]
        record["sha256_after"] = after["sha256"]
        record["identity_changed"] = (
            after["size"] != record["size_before"]
            or after["sha256"] != record["sha256_before"]
        )
    return record


def listing_payload(group: str) -> dict[str, Any]:
    names = select_group(group)
    return {
        "schema": SCHEMA_VERSION,
        "scope": SCOPE,
        "mode": "list",
        "group": group,
        "count": len(names),
        "scripts": list(names),
        "attempted": 0,
        "executed": 0,
        "verified": False,
        "note": (
            "Listing only: no test was imported or executed; nothing is claimed "
            "to pass and no verification is claimed."
        ),
    }


def build_summary(
    group: str,
    paths,
    problems: list[dict[str, str]],
    records: list[dict[str, Any]],
    output_path: Path | None = None,
) -> dict[str, Any]:
    selected = len(tuple(paths))
    attempted = sum(1 for r in records if r["attempted"])
    executed = sum(1 for r in records if r["executed"])
    passed = sum(1 for r in records if record_passed(r))
    failed = sum(1 for r in records if r["executed"] and r["returncode"] not in (0, None))
    timed_out = sum(1 for r in records if r["timed_out"])
    launch_errors = sum(1 for r in records if r["launch_error"] is not None)
    identity_changes = sum(1 for r in records if r["identity_changed"])
    ok = bool(not problems) and selected > 0 and executed == selected and passed == selected
    entry_identity = file_identity(Path(__file__))
    return {
        "schema": SCHEMA_VERSION,
        "scope": SCOPE,
        "mode": "run",
        "group": group,
        "ok": ok,
        "counts_unit": "scripts",
        "counts": {
            "selected": selected,
            "attempted": attempted,
            "executed": executed,
            "passed": passed,
            "failed": failed,
            "timed_out": timed_out,
            "launch_errors": launch_errors,
            "identity_changes": identity_changes,
        },
        "not_run": list(NOT_RUN),
        "limits": {
            "timeout_seconds_per_script": SCRIPT_TIMEOUT_SECONDS,
            "max_stream_bytes_per_stream": MAX_STREAM_BYTES,
            "limits_note": (
                "max_stream_bytes_per_stream only clips what is recorded in the "
                "summary; subprocess output capture is not a hard memory limit "
                "and not process-level memory protection."
            ),
            "shell": False,
            "parallel": False,
            "windows_creation_flags": (
                "CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS"
                if sys.platform == "win32"
                else "none"
            ),
        },
        "output": {
            "requested": output_path is not None,
            "path": str(output_path) if output_path is not None else None,
            "error": None,
        },
        "runner": {
            "python_executable": sys.executable,
            "python_version": sys.version,
            "platform": sys.platform,
            "tree_root": str(ROOT),
            "launch_cwd": str(Path.cwd()),
            "entry_path": ENTRY_RELATIVE_PATH,
            "entry_size": entry_identity["size"] if entry_identity else None,
            "entry_sha256": entry_identity["sha256"] if entry_identity else None,
        },
        "coverage_note": (
            "SHA-256 values identify only this entry and the selected whitelist "
            "scripts at capture time; they do not claim coverage of indirect "
            "dependencies or any assertion count. Existing native gates remain "
            "independent and their mains were not invoked by this entry."
        ),
        "problems": problems,
        "tests": records,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="run_architecture_python_checks.py",
        description=(
            "Run the frozen recorder/render-host pure-Python whitelist serially. "
            "Targeted script-level selection only; no auto-discovery, no native "
            "build, no GPU, no game."
        ),
    )
    parser.add_argument(
        "--group",
        choices=GROUP_CHOICES,
        default="all",
        help="Whitelist group to run (default: all).",
    )
    exclusive = parser.add_mutually_exclusive_group()
    exclusive.add_argument(
        "--list",
        action="store_true",
        help="Only print the selection as JSON; never import or execute tests.",
    )
    exclusive.add_argument(
        "--output",
        type=Path,
        metavar="NEW_JSON",
        help=(
            "Additionally write the summary JSON to this new file (CreateNew; "
            "refused if it already exists)."
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    names = select_group(args.group)
    if args.list:
        print(json.dumps(listing_payload(args.group), indent=2))
        return 0
    output_path = Path(args.output).resolve() if args.output is not None else None
    if output_path is not None and output_path.exists():
        print(
            "refusing: --output already exists: %s; no test was executed"
            % output_path,
            file=sys.stderr,
        )
        return 1
    paths = selection_paths(args.group)
    problems = verify_selection(paths)
    records: list[dict[str, Any]] = []
    if not problems:
        for path in paths:
            records.append(run_one(path))
    summary = build_summary(args.group, paths, problems, records, output_path)
    # The publication attempt happens BEFORE the final stdout payload is
    # printed, so an output failure is folded into the machine-readable
    # result (ok=false plus output.error) instead of leaving a success claim
    # on stdout.
    if output_path is not None:
        try:
            with open(output_path, "x", encoding="utf-8", newline="\n") as handle:
                handle.write(json.dumps(summary, indent=2))
                handle.write("\n")
        except OSError as exc:
            summary["output"]["error"] = "%s: %s: %s" % (
                output_path,
                type(exc).__name__,
                exc,
            )
            summary["ok"] = False
            print(
                "output publication failed; the run is not reported as success: "
                "%s: %s: %s" % (output_path, type(exc).__name__, exc),
                file=sys.stderr,
            )
        else:
            print("summary written to %s" % output_path, file=sys.stderr)
    print(json.dumps(summary, indent=2))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())