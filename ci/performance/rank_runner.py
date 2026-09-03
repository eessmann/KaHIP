"""Run one ParHIP MPI rank and record that rank's peak resident memory.

The launcher deliberately uses ``fork`` + ``execvpe`` instead of
``subprocess``.  MPI launchers commonly pass PMI/PMIx channels as inherited
file descriptors, and closing those descriptors between ``mpiexec`` and the
real rank can make MPI initialization fail or silently fall back.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import resource
import signal
import socket
import sys
import time
from typing import Mapping, Sequence


_RANK_VARIABLES = (
    "OMPI_COMM_WORLD_RANK",
    "PMI_RANK",
    "PMIX_RANK",
    "MV2_COMM_WORLD_RANK",
    "SLURM_PROCID",
)


def resolve_rank(environment: Mapping[str, str]) -> int:
    observed: dict[str, int] = {}
    for name in _RANK_VARIABLES:
        if name not in environment:
            continue
        text = environment[name]
        try:
            value = int(text, 10)
        except ValueError as error:
            raise ValueError(f"MPI rank variable {name} is not an integer") from error
        if value < 0:
            raise ValueError(f"MPI rank variable {name} is negative")
        observed[name] = value

    if not observed:
        raise ValueError("no MPI rank environment variable is available")
    values = set(observed.values())
    if len(values) != 1:
        details = ", ".join(f"{name}={value}" for name, value in observed.items())
        raise ValueError(f"MPI rank environment variables disagree: {details}")
    return next(iter(values))


def _exit_code(wait_status: int) -> tuple[int, int | None]:
    if os.WIFEXITED(wait_status):
        return os.WEXITSTATUS(wait_status), None
    if os.WIFSIGNALED(wait_status):
        signal_number = os.WTERMSIG(wait_status)
        return 128 + signal_number, signal_number
    return 125, None


def _rss_bytes(usage: resource.struct_rusage) -> int:
    maximum = int(usage.ru_maxrss)
    return maximum if sys.platform == "darwin" else maximum * 1024


def _write_metrics(path: Path, metrics: Mapping[str, object]) -> None:
    if path.exists():
        raise FileExistsError(f"rank metrics already exist: {path}")
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("x", encoding="utf-8") as output:
        json.dump(metrics, output, sort_keys=True, allow_nan=False)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    try:
        os.link(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _install_parent_death_signal(parent_pid: int) -> None:
    if not sys.platform.startswith("linux"):
        return
    libc = ctypes.CDLL(None, use_errno=True)
    pr_set_pdeathsig = 1
    if libc.prctl(pr_set_pdeathsig, signal.SIGTERM, 0, 0, 0) != 0:
        error_number = ctypes.get_errno()
        raise OSError(error_number, os.strerror(error_number))
    if os.getppid() != parent_pid:
        os.kill(os.getpid(), signal.SIGTERM)


def _cpu_affinity() -> tuple[list[int], str]:
    if hasattr(os, "sched_getaffinity"):
        return sorted(os.sched_getaffinity(0)), "sched_getaffinity"
    count = os.cpu_count() or 1
    return list(range(count)), "logical-cpu-fallback"


def run_rank(
    command: Sequence[str],
    *,
    metrics_directory: Path,
    environment: Mapping[str, str],
    preload: Path | None = None,
) -> int:
    if not command:
        raise ValueError("rank command must not be empty")
    if preload is not None and not preload.is_file():
        raise FileNotFoundError(f"rank preload library does not exist: {preload}")
    rank = resolve_rank(environment)
    metrics_directory.mkdir(parents=True, exist_ok=True)
    metrics_path = metrics_directory / f"rank-{rank}.json"
    started_ns = time.monotonic_ns()
    affinity, affinity_source = _cpu_affinity()
    parent_pid = os.getpid()
    child = os.fork()
    if child == 0:
        try:
            for signal_number in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
                signal.signal(signal_number, signal.SIG_DFL)
            _install_parent_death_signal(parent_pid)
            child_environment = dict(environment)
            if preload is not None:
                prior_preload = child_environment.get("LD_PRELOAD")
                child_environment["LD_PRELOAD"] = str(preload.resolve())
                if prior_preload:
                    child_environment["LD_PRELOAD"] += f":{prior_preload}"
            os.execvpe(command[0], list(command), child_environment)
        except BaseException as error:  # nothing may unwind across fork/exec
            message = f"cannot exec MPI rank command: {error}\n".encode(
                "utf-8", errors="replace"
            )
            try:
                os.write(2, message)
            finally:
                os._exit(127)

    previous_handlers = {}

    def forward_signal(signal_number: int, _frame: object) -> None:
        try:
            os.kill(child, signal_number)
        except ProcessLookupError:
            pass

    for signal_number in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        previous_handlers[signal_number] = signal.signal(
            signal_number, forward_signal
        )
    try:
        while True:
            try:
                _, wait_status, usage = os.wait4(child, 0)
                break
            except InterruptedError:
                continue
    finally:
        for signal_number, previous in previous_handlers.items():
            signal.signal(signal_number, previous)
    finished_ns = time.monotonic_ns()
    return_code, signal_number = _exit_code(wait_status)
    _write_metrics(
        metrics_path,
        {
            "schema_version": 1,
            "rank": rank,
            "hostname": socket.gethostname(),
            "child_pid": child,
            "return_code": return_code,
            "signal": signal_number,
            "elapsed_nanoseconds": finished_ns - started_ns,
            "max_rss_bytes": _rss_bytes(usage),
            "cpu_affinity": affinity,
            "cpu_affinity_source": affinity_source,
        },
    )
    return return_code


def main(arguments: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="launch one MPI rank without closing PMI/PMIx descriptors"
    )
    parser.add_argument("--metrics-directory", type=Path, required=True)
    parser.add_argument("--preload", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args(arguments)
    command = list(options.command)
    if command and command[0] == "--":
        command.pop(0)
    try:
        return run_rank(
            command,
            metrics_directory=options.metrics_directory,
            environment=os.environ,
            preload=options.preload,
        )
    except (OSError, ValueError) as error:
        print(f"rank measurement failed: {error}", file=sys.stderr)
        return 125


if __name__ == "__main__":
    raise SystemExit(main())
